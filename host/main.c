#include <stdio.h>

#include "app.h"
#include "console.h"
#include "instance.h"
#include "ui.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* A second launch hands its request to the copy already running, which
       raises its window; there is nothing left for this one to do. */
    if (!instance_acquire()) {
        return 0;
    }

    static app_t app;
    app_init(&app);

    /* app_init has read config.json, so "debug" is known. */
    console_init(app.cfg.debug);

    /* Try once immediately; the UI loop keeps retrying on a timer after. */
    app_connect(&app);

    int status = ui_run(&app);

    /* Re-enables any device left disabled, saves a dirty config, releases USB. */
    app_shutdown(&app);
    instance_release();
    console_shutdown();
    return status;
}
