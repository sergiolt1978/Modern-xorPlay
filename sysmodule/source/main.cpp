#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>

#include "../../third_party/dr_mp3.h"
#include "../../third_party/dr_wav.h"
#include "../../third_party/stb_vorbis.c"

#define INNER_HEAP_SIZE 0x800000
#define HOME_TITLE_ID 0x0100000000001000ULL

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define OUTPUT_FRAMES 4096
#define SOURCE_FRAMES 16384

#define MAX_TRACKS 128
#define MAX_PATH 512

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    fsdevMountSdmc();

    rc = pmdmntInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    rc = pminfoInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);
}

void __appExit(void)
{
    pminfoExit();
    pmdmntExit();
    fsdevUnmountAll();
    fsExit();
}

static void sleep_ms(u64 ms)
{
    svcSleepThread(ms * 1000000ULL);
}

static void trim(char* s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;

    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;

    if (start)
        memmove(s, s + start, strlen(s + start) + 1);
}

static int starts_with(const char* s, const char* p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static int file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void mkdir_if_missing(const char* path)
{
    mkdir(path, 0777);
}

static int hex_title(u64 title_id, char* out, size_t out_size)
{
    if (out_size < 17) return 0;
    snprintf(out, out_size, "%016llX", (unsigned long long)title_id);
    return 1;
}

class Decoder {
public:
    enum Type { NONE, MP3, WAV, OGG };

    Decoder() : type(NONE), vorbis(NULL), channels(0), sample_rate(0) {}

    bool open(const char* path)
    {
        close();

        const char* dot = strrchr(path, '.');
        if (!dot) return false;

        char ext[8] = {};
        snprintf(ext, sizeof(ext), "%s", dot + 1);
        for (size_t i = 0; ext[i]; i++) ext[i] = (char)tolower((unsigned char)ext[i]);

        if (!strcmp(ext, "mp3")) {
            if (!drmp3_init_file(&mp3, path, NULL))
                return false;
            type = MP3;
            channels = mp3.channels;
            sample_rate = mp3.sampleRate;
        }
        else if (!strcmp(ext, "wav")) {
            if (!drwav_init_file(&wav, path, NULL))
                return false;
            type = WAV;
            channels = wav.channels;
            sample_rate = wav.sampleRate;
            if (wav.translatedFormatTag != DR_WAVE_FORMAT_PCM &&
                wav.translatedFormatTag != DR_WAVE_FORMAT_IEEE_FLOAT)
                return false;
        }
        else if (!strcmp(ext, "ogg")) {
            int error = 0;
            vorbis = stb_vorbis_open_filename(path, &error, NULL);
            if (!vorbis) return false;
            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            channels = info.channels;
            sample_rate = info.sample_rate;
        }
        else {
            return false;
        }

        if (channels < 1 || channels > 2 || sample_rate == 0) {
            close();
            return false;
        }

        return true;
    }

    size_t read(float* out, size_t max_frames)
    {
        if (type == MP3)
            return (size_t)drmp3_read_pcm_frames_f32(&mp3, max_frames, out);

        if (type == WAV)
            return (size_t)drwav_read_pcm_frames_f32(&wav, max_frames, out);

        if (type == OGG)
            return (size_t)stb_vorbis_get_samples_float_interleaved(
                vorbis, (int)channels, out, (int)(max_frames * channels));

        return 0;
    }

    void close()
    {
        if (type == MP3)
            drmp3_uninit(&mp3);
        else if (type == WAV)
            drwav_uninit(&wav);
        else if (type == OGG && vorbis)
            stb_vorbis_close(vorbis);

        type = NONE;
        vorbis = NULL;
        channels = 0;
        sample_rate = 0;
    }

    ~Decoder() { close(); }

    unsigned channels;
    unsigned sample_rate;

private:
    Type type;
    drmp3 mp3;
    drwav wav;
    stb_vorbis* vorbis;
};

class Stream {
public:
    Stream() : source_count(0), source_pos(0.0), eof(false), active(false) {}

    bool open(const char* path)
    {
        close();
        if (!decoder.open(path))
            return false;

        if (decoder.channels > 2 || decoder.sample_rate == 0)
            return false;

        source_count = 0;
        source_pos = 0.0;
        eof = false;
        active = true;
        return true;
    }

    void close()
    {
        decoder.close();
        source_count = 0;
        source_pos = 0.0;
        eof = false;
        active = false;
    }

    bool finished() const
    {
        return active && eof && source_count < 2;
    }

    size_t read_stereo(float* out, size_t output_frames)
    {
        if (!active) return 0;

        const double ratio = (double)decoder.sample_rate / (double)SAMPLE_RATE;
        size_t produced = 0;

        while (produced < output_frames) {
            size_t i0 = (size_t)source_pos;
            size_t need = i0 + 2;

            if (source_count < need) {
                if (!fill_source(need)) {
                    if (source_count < 2 || i0 + 1 >= source_count)
                        break;
                }
            }

            if (i0 + 1 >= source_count)
                break;

            float l0, r0, l1, r1;

            if (decoder.channels == 1) {
                l0 = r0 = source[i0];
                l1 = r1 = source[i0 + 1];
            } else {
                l0 = source[i0 * 2 + 0];
                r0 = source[i0 * 2 + 1];
                l1 = source[(i0 + 1) * 2 + 0];
                r1 = source[(i0 + 1) * 2 + 1];
            }

            double frac = source_pos - (double)i0;
            out[produced * 2 + 0] = (float)(l0 + (l1 - l0) * frac);
            out[produced * 2 + 1] = (float)(r0 + (r1 - r0) * frac);

            source_pos += ratio;
            produced++;
        }

        size_t consumed = (size_t)source_pos;
        if (consumed > 0) {
            if (consumed > source_count) consumed = source_count;
            size_t remain = source_count - consumed;

            if (remain > 0)
                memmove(source, source + consumed * decoder.channels,
                        remain * decoder.channels * sizeof(float));

            source_count = remain;
            source_pos -= (double)consumed;
        }

        return produced;
    }

private:
    bool fill_source(size_t wanted)
    {
        while (source_count < wanted && source_count < SOURCE_FRAMES) {
            size_t room = SOURCE_FRAMES - source_count;
            size_t got = decoder.read(
                source + source_count * decoder.channels,
                room
            );

            if (got == 0) {
                eof = true;
                break;
            }

            source_count += got;
        }

        return source_count >= wanted;
    }

    Decoder decoder;
    float source[SOURCE_FRAMES * 2];
    size_t source_count;
    double source_pos;
    bool eof;
    bool active;
};

class Playlist {
public:
    Playlist() : count(0), index(0), repeat(1), shuffle(false) {}

    void clear()
    {
        count = 0;
        index = 0;
    }

    bool load_file(const char* path, const char* base_dir)
    {
        clear();

        FILE* f = fopen(path, "r");
        if (!f) return false;

        char line[MAX_PATH];

        while (fgets(line, sizeof(line), f) && count < MAX_TRACKS) {
            trim(line);

            if (!line[0] || line[0] == '#')
                continue;

            if (starts_with(line, "sdmc:/")) {
                snprintf(tracks[count], MAX_PATH, "%s", line);
            } else if (line[0] == '/') {
                snprintf(tracks[count], MAX_PATH, "sdmc:%s", line);
            } else {
                snprintf(tracks[count], MAX_PATH, "%s/%s", base_dir, line);
            }

            count++;
        }

        fclose(f);
        return count > 0;
    }

    bool load_for_title(u64 title_id)
    {
        char id[32];
        hex_title(title_id, id, sizeof(id));

        char dir[MAX_PATH];
        char playlist[MAX_PATH];

        if (title_id == 0) {
            snprintf(dir, sizeof(dir), "sdmc:/music/home");
            snprintf(playlist, sizeof(playlist), "%s/playlist.txt", dir);
            if (load_file(playlist, dir))
                return true;
        } else {
            snprintf(dir, sizeof(dir), "sdmc:/music/%s", id);
            snprintf(playlist, sizeof(playlist), "%s/playlist.txt", dir);
            if (load_file(playlist, dir))
                return true;
        }

        snprintf(dir, sizeof(dir), "sdmc:/music/default");
        snprintf(playlist, sizeof(playlist), "%s/playlist.txt", dir);
        if (load_file(playlist, dir))
            return true;

        return load_legacy(title_id);
    }

    bool load_legacy(u64 title_id)
    {
        FILE* f = fopen("sdmc:/xor.play.json", "r");
        if (f) {
            char line[1024];
            char wanted[32];
            hex_title(title_id == 0 ? HOME_TITLE_ID : title_id, wanted, sizeof(wanted));

            while (fgets(line, sizeof(line), f)) {
                char key[32] = {};
                char value[MAX_PATH] = {};

                if (sscanf(line, " \"%31[0-9A-Fa-f]\" : \"%511[^\"]\"", key, value) == 2 ||
                    sscanf(line, " \"%31[0-9A-Fa-f]\" : \"%511[^\"]\",", key, value) == 2) {

                    if (!strcasecmp(key, wanted)) {
                        snprintf(tracks[0], MAX_PATH, "%s", value);
                        count = 1;
                        fclose(f);
                        return true;
                    }
                }
            }

            fclose(f);
        }

        f = fopen("sdmc:/sys-play.txt", "r");
        if (f) {
            clear();
            char line[MAX_PATH];

            while (fgets(line, sizeof(line), f) && count < MAX_TRACKS) {
                trim(line);
                if (!line[0] || line[0] == '#')
                    continue;

                if (starts_with(line, "sdmc:/"))
                    snprintf(tracks[count], MAX_PATH, "%s", line);
                else
                    snprintf(tracks[count], MAX_PATH, "sdmc:/media/%s", line);

                count++;
            }

            fclose(f);
            return count > 0;
        }

        return false;
    }

    const char* current() const
    {
        if (!count) return NULL;
        return tracks[index];
    }

    bool next()
    {
        if (!count) return false;

        if (repeat == 2)
            return true;

        if (shuffle && count > 1) {
            u32 r = next_random();
            index = r % count;
            return true;
        }

        if (index + 1 < count) {
            index++;
            return true;
        }

        if (repeat == 1) {
            index = 0;
            return true;
        }

        return false;
    }

    bool previous()
    {
        if (!count) return false;

        if (index > 0) {
            index--;
            return true;
        }

        if (repeat == 1) {
            index = count - 1;
            return true;
        }

        return false;
    }

    void set_repeat(int mode)
    {
        if (mode < 0 || mode > 2) mode = 1;
        repeat = mode;
    }

    void set_shuffle(bool value)
    {
        shuffle = value;
    }

    size_t size() const { return count; }
    size_t position() const { return index; }

private:
    u32 next_random()
    {
        static u32 seed = 0x51A7C0DE;
        seed = seed * 1664525u + 1013904223u;
        return seed;
    }

    char tracks[MAX_TRACKS][MAX_PATH];
    size_t count;
    size_t index;
    int repeat;
    bool shuffle;
};

class AudioOutput {
public:
    AudioOutput() : opened(false), paused(false), volume(1.0f)
    {
        memset(&buffer, 0, sizeof(buffer));
    }

    bool init()
    {
        Result rc = audoutInitialize();
        if (R_FAILED(rc))
            return false;

        u32 in_rate = SAMPLE_RATE;
        u32 in_channels = CHANNELS;
        u32 out_rate = 0;
        u32 out_channels = 0;
        PcmFormat format = PcmFormat_Int16;
        AudioOutState state = AudioOutState_Stopped;
        char device[0x100] = {};

        rc = audoutOpenAudioOut(
            NULL, device,
            in_rate, in_channels,
            &out_rate, &out_channels,
            &format, &state
        );

        if (R_FAILED(rc)) {
            audoutExit();
            return false;
        }

        if (out_rate != SAMPLE_RATE || out_channels != CHANNELS || format != PcmFormat_Int16) {
            audoutExit();
            return false;
        }

        buffer.next = NULL;
        buffer.buffer = pcm;
        buffer.buffer_size = sizeof(pcm);
        buffer.data_size = sizeof(pcm);
        buffer.data_offset = 0;

        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            audoutExit();
            return false;
        }

        opened = true;
        return true;
    }

    void exit()
    {
        if (opened) {
            audoutStopAudioOut();
            audoutExit();
            opened = false;
        }
    }

    void set_volume(float v)
    {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        volume = v;
    }

    bool play(Stream& stream)
    {
        if (!opened || paused)
            return false;

        float temp[OUTPUT_FRAMES * 2];
        size_t frames = stream.read_stereo(temp, OUTPUT_FRAMES);

        if (frames == 0)
            return false;

        size_t samples = frames * 2;

        for (size_t i = 0; i < samples; i++) {
            float x = temp[i] * volume;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            pcm[i] = (int16_t)(x * 32767.0f);
        }

        size_t bytes = frames * CHANNELS * sizeof(int16_t);

        if (bytes < sizeof(pcm))
            memset(((u8*)pcm) + bytes, 0, sizeof(pcm) - bytes);

        buffer.data_size = sizeof(pcm);

        AudioOutBuffer* released = NULL;
        Result rc = audoutPlayBuffer(&buffer, &released);
        return R_SUCCEEDED(rc);
    }

    bool paused;
    float volume;

private:
    bool opened;
    alignas(0x1000) int16_t pcm[OUTPUT_FRAMES * CHANNELS];
    AudioOutBuffer buffer;
};

class XorPlay {
public:
    XorPlay()
        : title_id(0), last_title_id(UINT64_MAX),
          enabled(true), volume(1.0f), shuffle(false),
          repeat(1), reload_requested(true), paused(false)
    {
    }

    void init()
    {
        mkdir_if_missing("sdmc:/config");
        mkdir_if_missing("sdmc:/config/xorplay");

        load_config();

        if (!audio.init()) {
            write_status("audio_init_failed");
            return;
        }

        apply_title(0);
    }

    void shutdown()
    {
        audio.exit();
    }

    void tick()
    {
        process_command();

        if (reload_requested) {
            load_config();
            reload_requested = false;
            apply_title(last_title_id == UINT64_MAX ? 0 : last_title_id);
        }

        u64 current = detect_title_id();

        if (current != last_title_id) {
            apply_title(current);
            last_title_id = current;
        }

        if (!enabled || paused || !playlist.size()) {
            write_status("idle");
            sleep_ms(10);
            return;
        }

        if (!stream_active) {
            if (!open_current_track()) {
                if (!advance_track()) {
                    paused = true;
                    write_status("playlist_finished");
                    return;
                }
            }
        }

        if (!audio.play(stream)) {
            if (stream.finished()) {
                stream_active = false;

                if (!advance_track()) {
                    if (repeat == 0) {
                        paused = true;
                        write_status("finished");
                        return;
                    }

                    paused = true;
                    write_status("finished");
                    return;
                }
            } else {
                sleep_ms(2);
            }
        }

        write_status("playing");
    }

private:
    u64 detect_title_id()
    {
        u64 pid = 0;
        if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0)
            return 0;

        u64 program_id = 0;
        if (R_FAILED(pminfoGetProgramId(&program_id, pid)))
            return 0;

        return program_id;
    }

    void apply_title(u64 id)
    {
        title_id = id;
        stream_active = false;
        stream.close();

        playlist.set_repeat(repeat);
        playlist.set_shuffle(shuffle);

        if (!playlist.load_for_title(id)) {
            playlist.clear();
        }

        paused = !enabled;
    }

    bool open_current_track()
    {
        const char* path = playlist.current();
        if (!path)
            return false;

        if (!file_exists(path))
            return false;

        if (!stream.open(path))
            return false;

        stream_active = true;
        return true;
    }

    bool advance_track()
    {
        stream.close();
        stream_active = false;

        if (!playlist.next())
            return false;

        return open_current_track();
    }

    void process_command()
    {
        FILE* f = fopen("sdmc:/config/xorplay/command.txt", "r");
        if (!f)
            return;

        char cmd[256] = {};
        fgets(cmd, sizeof(cmd), f);
        fclose(f);
        remove("sdmc:/config/xorplay/command.txt");

        trim(cmd);

        if (!strcmp(cmd, "pause")) {
            paused = true;
            audio.paused = true;
        }
        else if (!strcmp(cmd, "resume")) {
            paused = false;
            audio.paused = false;
        }
        else if (!strcmp(cmd, "toggle")) {
            paused = !paused;
            audio.paused = paused;
        }
        else if (!strcmp(cmd, "next")) {
            stream.close();
            stream_active = false;
            if (!playlist.next())
                playlist.clear();
        }
        else if (!strcmp(cmd, "previous")) {
            stream.close();
            stream_active = false;
            playlist.previous();
        }
        else if (!strcmp(cmd, "reload")) {
            reload_requested = true;
        }
        else if (!strcmp(cmd, "shuffle")) {
            shuffle = !shuffle;
            playlist.set_shuffle(shuffle);
        }
        else if (!strcmp(cmd, "repeat")) {
            repeat++;
            if (repeat > 2) repeat = 0;
            playlist.set_repeat(repeat);
        }
        else if (!strcmp(cmd, "volume_up")) {
            volume += 0.05f;
            if (volume > 1.0f) volume = 1.0f;
            audio.set_volume(volume);
        }
        else if (!strcmp(cmd, "volume_down")) {
            volume -= 0.05f;
            if (volume < 0.0f) volume = 0.0f;
            audio.set_volume(volume);
        }
        else if (starts_with(cmd, "volume=")) {
            volume = (float)atof(cmd + 7);
            if (volume < 0.0f) volume = 0.0f;
            if (volume > 1.0f) volume = 1.0f;
            audio.set_volume(volume);
        }
        else if (!strcmp(cmd, "enable")) {
            enabled = true;
            paused = false;
        }
        else if (!strcmp(cmd, "disable")) {
            enabled = false;
            paused = true;
            audio.paused = true;
        }
    }

    void load_config()
    {
        FILE* f = fopen("sdmc:/config/xorplay/config.ini", "r");
        if (!f)
            return;

        char line[256];
        char section[64] = {};

        while (fgets(line, sizeof(line), f)) {
            trim(line);

            if (!line[0] || line[0] == '#')
                continue;

            if (line[0] == '[') {
                sscanf(line, "[%63[^]]", section);
                continue;
            }

            if (strcmp(section, "global") != 0)
                continue;

            char key[64] = {};
            char value[128] = {};

            if (sscanf(line, "%63[^=]=%127s", key, value) != 2)
                continue;

            trim(key);
            trim(value);

            if (!strcmp(key, "enabled"))
                enabled = atoi(value) != 0;
            else if (!strcmp(key, "volume"))
                volume = (float)atof(value);
            else if (!strcmp(key, "shuffle"))
                shuffle = atoi(value) != 0;
            else if (!strcmp(key, "repeat"))
                repeat = atoi(value);
            else if (!strcmp(key, "poll_ms"))
                poll_ms = (u32)atoi(value);
        }

        fclose(f);

        if (repeat < 0 || repeat > 2) repeat = 1;
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;

        audio.set_volume(volume);
        audio.paused = paused;
    }

    void write_status(const char* state)
    {
        FILE* f = fopen("sdmc:/config/xorplay/status.txt", "w");
        if (!f) return;

        char id[32];
        hex_title(title_id == 0 ? HOME_TITLE_ID : title_id, id, sizeof(id));

        const char* track = playlist.current();
        if (!track) track = "";

        fprintf(f, "state=%s\n", state);
        fprintf(f, "title_id=%s\n", id);
        fprintf(f, "track=%s\n", track);
        fprintf(f, "position=%u\n", (unsigned)(playlist.position() + 1));
        fprintf(f, "count=%u\n", (unsigned)playlist.size());
        fprintf(f, "paused=%d\n", paused ? 1 : 0);
        fprintf(f, "enabled=%d\n", enabled ? 1 : 0);
        fprintf(f, "volume=%.2f\n", volume);
        fprintf(f, "shuffle=%d\n", shuffle ? 1 : 0);
        fprintf(f, "repeat=%d\n", repeat);

        fclose(f);
    }

    AudioOutput audio;
    Stream stream;
    Playlist playlist;

    u64 title_id;
    u64 last_title_id;

    bool enabled;
    float volume;
    bool shuffle;
    int repeat;
    u32 poll_ms = 500;

    bool reload_requested;
    bool paused;
    bool stream_active = false;
};

int main(int, char**)
{
    XorPlay player;
    player.init();

    while (true) {
        player.tick();
    }

    player.shutdown();
    return 0;
}
