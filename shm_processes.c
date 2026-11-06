/*
 * shm_processes.c
 * Poor Student - Dear Old Dad problem (with Lovable Mom + N Poor Students
 * extra credit) solved with shared memory and POSIX semaphores.
 *
 * Usage:
 *   ./shm_processes            -> 1 parent (Dear Old Dad), 1 Poor Student
 *   ./shm_processes 1 3        -> Dear Old Dad, 3 Poor Students
 *   ./shm_processes 2 10       -> Dear Old Dad + Lovable Mom, 10 Poor Students
 *
 * Shared data: one int BankAccount plus one semaphore (mutex) that guards
 * every read-modify-write of BankAccount so no race conditions occur.
 * Kill the program with Ctrl-C or by sending a signal to the parent process
 * (ps -eaf / kill <pid>); the parent cleans up every child.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <semaphore.h>

/* Everything that must be shared between the processes lives here. */
typedef struct {
    int   BankAccount;   /* the shared bank account                     */
    sem_t mutex;         /* binary semaphore: guards BankAccount        */
} SharedData;

static SharedData *shared = NULL;   /* mapped shared memory              */
static pid_t      *kids   = NULL;   /* pids of forked children (parent)  */
static int         nkids  = 0;

/* ------------------------------------------------------------------ */
/* Clean-up: parent kills all children, destroys semaphore, unmaps.    */
/* ------------------------------------------------------------------ */
static void cleanup_and_exit(int sig)
{
    (void)sig;
    for (int i = 0; i < nkids; i++)
        if (kids[i] > 0) kill(kids[i], SIGTERM);
    for (int i = 0; i < nkids; i++)
        if (kids[i] > 0) waitpid(kids[i], NULL, 0);

    if (shared != NULL) {
        sem_destroy(&shared->mutex);
        munmap(shared, sizeof(SharedData));
    }
    free(kids);
    printf("\nAll processes terminated.\n");
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Dear Old Dad                                                        */
/* ------------------------------------------------------------------ */
static void DearOldDad(void)
{
    int localBalance = 0;
    int randomNumber, amount;

    srand((unsigned)(time(NULL) ^ getpid()));

    while (1) {
        sleep(rand() % 6);                                  /* 0 - 5 s  */
        printf("Dear Old Dad: Attempting to Check Balance\n");

        randomNumber = rand();

        if (randomNumber % 2 == 0) {
            if (localBalance < 100) {
                /* ---- Deposit Money ---- */
                sem_wait(&shared->mutex);                   /* DOWN     */
                localBalance = shared->BankAccount;         /* copy in  */
                amount = rand() % 101;                      /* $0-$100  */
                if (amount % 2 == 0) {
                    localBalance += amount;
                    printf("Dear old Dad: Deposits $%d / Balance = $%d\n",
                           amount, localBalance);
                    shared->BankAccount = localBalance;     /* copy out */
                } else {
                    printf("Dear old Dad: Doesn't have any money to give\n");
                }
                sem_post(&shared->mutex);                   /* UP       */
            } else {
                printf("Dear old Dad: Thinks Student has enough Cash ($%d)\n",
                       localBalance);
            }
        } else {
            /* ---- Just check the balance ---- */
            sem_wait(&shared->mutex);
            localBalance = shared->BankAccount;
            sem_post(&shared->mutex);
            printf("Dear Old Dad: Last Checking Balance = $%d\n", localBalance);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lovable Mom (extra credit)                                          */
/* ------------------------------------------------------------------ */
static void LovableMom(void)
{
    int localBalance = 0;
    int amount;

    srand((unsigned)(time(NULL) ^ getpid()));

    while (1) {
        sleep(rand() % 11);                                 /* 0 - 10 s */
        printf("Loveable Mom: Attempting to Check Balance\n");

        if (localBalance <= 100) {
            /* ---- Deposit Money (always) ---- */
            sem_wait(&shared->mutex);
            localBalance = shared->BankAccount;
            amount = rand() % 126;                          /* $0-$125  */
            localBalance += amount;
            printf("Lovable Mom: Deposits $%d / Balance = $%d\n",
                   amount, localBalance);
            shared->BankAccount = localBalance;
            sem_post(&shared->mutex);
        } else {
            /* Balance looks healthy: refresh the local copy for next time */
            sem_wait(&shared->mutex);
            localBalance = shared->BankAccount;
            sem_post(&shared->mutex);
            printf("Lovable Mom: Thinks Student has enough Cash ($%d)\n",
                   localBalance);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Poor Student                                                        */
/* ------------------------------------------------------------------ */
static void PoorStudent(void)
{
    int localBalance = 0;
    int randomNumber, need;

    srand((unsigned)(time(NULL) ^ getpid()));

    while (1) {
        sleep(rand() % 6);                                  /* 0 - 5 s  */
        printf("Poor Student: Attempting to Check Balance\n");

        randomNumber = rand();

        if (randomNumber % 2 == 0) {
            /* ---- Withdraw Money ---- */
            sem_wait(&shared->mutex);                       /* DOWN     */
            localBalance = shared->BankAccount;             /* copy in  */
            need = rand() % 51;                             /* $0-$50   */
            printf("Poor Student needs $%d\n", need);
            if (need <= localBalance) {
                localBalance -= need;
                printf("Poor Student: Withdraws $%d / Balance = $%d\n",
                       need, localBalance);
                shared->BankAccount = localBalance;         /* copy out */
            } else {
                printf("Poor Student: Not Enough Cash ($%d)\n", localBalance);
            }
            sem_post(&shared->mutex);                       /* UP       */
        } else {
            /* ---- Just check the balance ---- */
            sem_wait(&shared->mutex);
            localBalance = shared->BankAccount;
            sem_post(&shared->mutex);
            printf("Poor Student: Last Checking Balance = $%d\n", localBalance);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int numParents  = 1;   /* 1 = Dad only, 2 = Dad + Mom */
    int numStudents = 1;

    if (argc >= 3) {
        numParents  = atoi(argv[1]);
        numStudents = atoi(argv[2]);
    } else if (argc == 2) {
        numParents  = atoi(argv[1]);
    }
    if (numParents  < 1) numParents  = 1;
    if (numParents  > 2) numParents  = 2;
    if (numStudents < 1) numStudents = 1;

    /* Unbuffered stdout so output from every process appears immediately
       and buffered text is not duplicated by fork(). */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- Create the shared memory (BankAccount + semaphore) ---- */
    shared = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    shared->BankAccount = 0;

    /* ---- Create the semaphore shared between processes (pshared=1),
            initial value 1 => mutual exclusion ---- */
    if (sem_init(&shared->mutex, 1, 1) == -1) {
        perror("sem_init");
        exit(1);
    }

    /* ---- Fork the children: Mom (optional) + N students ---- */
    nkids = (numParents == 2 ? 1 : 0) + numStudents;
    kids  = calloc(nkids, sizeof(pid_t));
    int k = 0;

    /* Parent handles termination signals and cleans up the children */
    signal(SIGINT,  cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGHUP,  cleanup_and_exit);

    if (numParents == 2) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
            LovableMom();
            _exit(0);
        }
        kids[k++] = pid;
    }

    for (int i = 0; i < numStudents; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
            PoorStudent();
            _exit(0);
        }
        kids[k++] = pid;
    }

    /* The original (parent) process is Dear Old Dad */
    DearOldDad();

    /* never reached - loop is infinite; cleanup happens in the handler */
    cleanup_and_exit(0);
    return 0;
}