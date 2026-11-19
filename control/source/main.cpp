#include <switch.h>

#include <stdio.h>
#include <string.h>

static const char* COMMAND_PATH = "sdmc:/config/xorplay/command.txt";
static const char* STATUS_PATH = "sdmc:/config/xorplay/status.txt";

static void send_command(const char* command)
{
    FILE* f = fopen(COMMAND_PATH, "w");
    if (!f) return;
    fprintf(f, "%s\n", command);
    fclose(f);
}

static void show_status()
{
    FILE* f = fopen(STATUS_PATH, "r");
    if (!f) {
        printf("No status file yet.\n");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), f))
        printf("%s", line);

    fclose(f);
}

int main(int argc, char** argv)
{
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    fsInitialize();
    fsdevMountSdmc();

    while (appletMainLoop())
    {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_A) send_command("toggle");
        if (down & HidNpadButton_B) send_command("next");
        if (down & HidNpadButton_X) send_command("previous");
        if (down & HidNpadButton_Y) send_command("reload");
        if (down & HidNpadButton_L) send_command("volume_down");
        if (down & HidNpadButton_R) send_command("volume_up");
        if (down & HidNpadButton_Down) send_command("shuffle");
        if (down & HidNpadButton_Up) send_command("repeat");

        consoleClear();
        printf("XorPlay Modern Controller\n\n");
        printf("A     Play/Pause\n");
        printf("B     Next\n");
        printf("X     Previous\n");
        printf("Y     Reload\n");
        printf("L/R   Volume -/+\n");
        printf("UP    Repeat mode\n");
        printf("DOWN  Shuffle\n");
        printf("+     Exit\n\n");

        printf("---- STATUS ----\n");
        show_status();

        consoleUpdate(NULL);
        svcSleepThread(100000000ULL);
    }

    fsdevUnmountAll();
    fsExit();
    consoleExit(NULL);
    return 0;
}
