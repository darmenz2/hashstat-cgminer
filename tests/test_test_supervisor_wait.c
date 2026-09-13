
#define main supervisor_main
#include "../runtime/hs_test_supervisor.c"
#undef main

int main(void)
{
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) _exit(23);
    for (int i = 0; i < 100; ++i) {
        int done = 0;
        int observed = observe_child(child, &done);
        if (observed || done) {
            printf("observe result=%d errno=%d done=%d\n", observed, errno, done);
            uint64_t now;
            int status = 0;
            if (clock_ms(&now)) return 2;
            int reaped = final_reap(child, now + HS_REAP_GRACE_MS, &status);
            printf("reap result=%d errno=%d status=%d\n", reaped, errno, status);
            return reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 23;
        }
        (void)poll(NULL, 0, 10);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
    return 3;
}
