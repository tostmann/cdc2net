// Task-WDT hooks for the main task.
//
// app_main() subscribes itself to the Task-WDT and feeds it once per second
// (see main.c).  That works as long as the main task actually gets scheduled —
// it runs at CONFIG_ESP_MAIN_TASK_PRIORITY (default 1), below wifi, tcpip and
// httpd, so a long transfer that keeps those busy can starve it past the 5 s
// timeout even though nothing is wedged.  Measured during a ~19 s OTA upload:
// TASK_WDT panic on `main` with the wifi task holding CPU 0.
//
// Wrap such a known, bounded operation in pause/resume instead of widening the
// timeout for everyone.  Both calls are no-ops if the main task never made it
// onto the watchdog.  Nesting is not supported — pause once, resume once.

#pragma once

// Callable from any task.  Neither is counted: pause once, resume once — today
// that holds only because a single httpd task performs the one operation that
// uses them.
void app_wdt_pause_main(void);   // unsubscribe main from the Task-WDT
void app_wdt_resume_main(void);  // clear the pause; the main loop re-subscribes
                                 // and feeds on its next tick (<= 1 s), because
                                 // only the task itself can feed itself
