/* Test-only clock: identical to clk.c but one "second" lasts TICK_US microseconds
 * (env var, default 1000000), so test cases run quickly. */
#include "../headers.h"
int shmid;
void cleanup(int signum) { shmctl(shmid, IPC_RMID, NULL); exit(0); }
int main(void)
{
    signal(SIGINT, cleanup);
    const char *e = getenv("TICK_US");
    useconds_t tick = e ? (useconds_t)atoi(e) : 1000000;
    shmid = shmget(SHKEY, 4, IPC_CREAT | 0644);
    if (shmid == -1) { perror("clk shmget"); exit(-1); }
    int *shmaddr = (int *)shmat(shmid, NULL, 0);
    *shmaddr = 0;
    while (1) { usleep(tick); (*shmaddr)++; }
}
