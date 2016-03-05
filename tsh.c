/* 
 * tsh - A tiny shell program with job control
 * 
 * <Zarana Parekh 201301177@daiict.ac.in>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

int check_fg; /* to check if the process is in the foreground state. */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {	
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }
    
    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 
    
    /* Initialize the job list */
    initjobs(jobs);
    /* Execute the shell's read/eval loop */
    while (1) {
	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/

void eval(char *cmdline) 
{
	char *argv[MAXARGS]; /* argv stores the arguments of cmdline in an array */
	int is_bg = parseline(cmdline,argv); /* parse cmdline and check if new job is FG or BG */
	pid_t pid = 0;
	sigset_t sSet; /* signal set */
	
	if(argv[0] == NULL) /* ignore empty lines */
		return;
		
	int is_builtin_cmd = builtin_cmd(argv); /* checking if cmdline is a built-in command */
	
	/* 
	Executing commands which are not built-in requires a new child process to be created using fork and executing the corresponding command using execve() function. 
	
	The signal SIGCHLD is blocked before adding a new job to the job list and unblocked later. This is done to ensure correct sequence of execution and that there is no race condition while adding or deleting a job which are the critical section of the code.
	   
	If SIGCHLD is not blocked, we may have the job being deleted from the job list (and the child being reaped) in the SIGCHLD handler even before being added to the job list due to race condition.
	   
	The child inherits the blocked vector of the parent, hence it must unblock SIGCHLD before executing the command.
	   */
	if(!is_builtin_cmd) {
		sigemptyset(&sSet); /* initialising signal set */
		sigaddset(&sSet,SIGCHLD); /* adding SIGCHLD to the signal set */
		/* blocking the SIGCHLD signal using sigprocmask without affecting other signals */
		if(sigprocmask(SIG_BLOCK,&sSet,NULL) < 0)
			unix_error("sigprocmask error\n");
		pid = fork();
		/* check if fork() was unsuccessful and child process has not been created */
		if(pid < 0)
			unix_error("fork error\n");
		if(pid == 0) { /* child runs the user job */
		/*
			Initially when the child process is forked, it inherits the process group ID of the parent process. Hence if SIGTSTP or SIGINT is sent to the child process, the parent process being in the same process group also receives the signal and will be stopped or terminated respectively. Hence setpgid(0,0) function is used to set the process group ID of child process to its PID so that the parent process is not stopped or terminated due to the corresponding signal being sent to the child process.	
		*/
			setpgid(0,0);
			/* unblocking SIGCHLD signal using sigprocmask */
			if(sigprocmask(SIG_UNBLOCK,&sSet,NULL) < 0)
				unix_error("sigprocmask error\n");
			/* executing the command using execve() */
			if(execve(argv[0],argv,environ) < 0) { 
				printf("%s: Command not found\n", argv[0]);
				exit(0);
			}	
		}
		if(!is_bg) { 
		/*
			If the job to be executed is a foreground job, then add it to the joblist with state being 'FG'(i.e. foreground) and unblock the SIGCHLD signal.
			
			waitfg is called then to ensure that there is only one job running in the foreground.
		*/
				addjob(jobs,pid,FG,cmdline); /* add job to the joblist */
				/* unblocking SIGCHLD signal using sigprocmask */
				if(sigprocmask(SIG_UNBLOCK,&sSet,NULL) < 0)
					unix_error("sigprocmask error\n");
				waitfg(pid); /* ensuring only 1 foreground process is there */
		} else {
		/*
			If the job to be executed is a background job, then add it to the joblist with state being 'BG'(i.e. background) and unblock the SIGCHLD signal.
			
			There can be multible jobs running in the background. Hence, we do have to wait for the job to terminate before adding another background job.
		*/
			addjob(jobs,pid,BG,cmdline); /* add job to the joblist */
			printf("[%d] (%d) %s", pid2jid(pid),pid,cmdline); 
			/* unblocking SIGCHLD signal using sigprocmask */
			if(sigprocmask(SIG_UNBLOCK,&sSet,NULL) < 0)
				unix_error("sigprocmask error\n");
		}
	}
	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
 
/*
	If first argument in cmdline is a built in command, run it and return.
	
	There are 4 built-in commands - quit, jobs, fg, bg. These commands must be executed immediately.
	
	return value: 0 - if cmdline is not a built-in command
	1 - if cmdline is a built-in command. 
	(cmdline is stored in argv after parsing and builtin_cmd accesses argv to check if cmdline is a built-in command.)
*/
int builtin_cmd(char **argv) 
{
	/* quit the process immediately using exit() function */
	if(!strcmp(*argv, "quit")) { 
		exit(0);
	}
	
	/* list the jobs in joblist */
	if(!strcmp(*argv, "jobs")) { 
		listjobs(jobs); 
		return 1;
	}
	
	/* resuming a stopped process in FG or BG */
	if(!strcmp(*argv, "fg") || !strcmp(*argv, "bg")) { 
		do_bgfg(argv);
		return 1;
	}
	
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
	/* checking the error conditions */
	
	/* fg/bg must be followed by value of job id or process id. */
	if(argv[1] == NULL) {
		printf("%s requires PID or %%jobid argument\n",*argv);
		return;
	}
	
	int arg = atoi(argv[1]+1); /* converting the job/process id to int using atoi. */
	
	/* the value of argument must be a valid integer corresponding to jid or pid. */
	if((argv[1][0] < '0' || argv[1][0] > '9') && argv[1][0] != '%') {
		printf("%s: argument must be a PID or %%jobid\n",*argv);
		return;
	}
	
	/* the argument value must correspond to some job in the joblist. */
	if(argv[1][0] == '%') {
		if(getjobjid(jobs,arg) == NULL) {
			printf("%%%d: No such job\n",arg);
			return;
		}
	}
	
	/* the argument value must correspond to some job in the joblist. */
	if(argv[1][0] != '%' && getjobpid(jobs,arg) == NULL) {
		printf("(%d): No such process\n",arg);
		return;
	}
	
	/* end of error handling section. */
	
	/*
		When the bg command is executed,the stopped process resumes execution on receiveing the SIGCONT signal and runs in the background.
		
		bg maybe followed by job id or process id so that needs to be checked first before changing the status of any job.
		
		After sending the SIGCONT signal, the state of the job is now background(i.e. BG).
	*/
	if(!strcmp(*argv,"bg")) {
		int jid = atoi(*(argv+1)+1); /* converting argument to int */
		
		/* if jid of the job is mentioned in the command, it is preceded by '%'. */
		if(argv[1][0] == '%') {
			kill(-(jobs[jid-1].pid),SIGCONT); /* sending SIGCONT to the job */
			jobs[jid-1].state = BG; /* change status of job to 'BG' */
			printf("[%d] (%d) %s",jid,jobs[jid-1].pid,jobs[jid-1].cmdline);			
		}
		
		/* if pid of the job is mentioned in the command.*/
		else {
			pid_t pid = jid;
			jid = pid2jid(pid); /* obtaining jid given pid of process */
			kill(-pid,SIGCONT); /* sending SIGCONT to the job */
			jobs[jid-1].state = BG; /* change status of job to 'BG' */
			printf("[%d] (%d) %s",jid,pid,jobs[jid-1].cmdline);
		}
	}
	
	/*
		When the fg command is executed,the stopped process resumes execution on receiveing the SIGCONT signal and runs in the foreground.
		
		fg maybe followed by job id or process id so that needs to be checked first before changing the status of any job.
		
		After sending the SIGCONT signal, the state of the job is now foreground(i.e. FG).
		
		Since the resumed process is now running in the foreground, waitfg() function is called to ensure that there is only foreground process being executed at any time.
	*/
	else if(!strcmp(*argv,"fg")) {
		int jid = atoi(*(argv+1)+1); /* converting argument into int */
		pid_t pid = 0;
		
		/* if jid of the job is mentioned in the command, it is preceded by '%'. */
		if(argv[1][0] == '%') {
			pid = jobs[jid-1].pid; /* obtain pid of the job from joblist using jid */
			kill(-(jobs[jid-1].pid),SIGCONT); /* sending SIGCONT to the job */ 
			jobs[jid-1].state = FG; /* change status of job to 'FG' */
		}
		
		/* if pid of the job is mentioned in the command.*/
		else {
			pid_t pid = jid;
			jid = pid2jid(pid); /* obtaining jid given pid of process */
			kill(-pid,SIGCONT); /* sending SIGCONT to the job */
			jobs[jid-1].state = FG; /* change status of job to 'FG'*/
		}	
		waitfg(pid);
	}
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
 
/*
	waitfg() ensures that there is only one foreground process being executed at any time. This is done by bolcking furher execution until the foreground process running (as given by the pid argument) i.terminates or ii.stops and shifts to the background.
	
	Hence, waitfg() is called each time a new process is added in the foreground(FG) state.
	
	check_fg is set when a foreground process receives a signal. In case a foreground job is not yet terminated, then pause again until the foreground process terminates. If the foreground process terminates then the if condition ensures that the shell does not pause again for any other process to terminate(i.e. background process).
*/
void waitfg(pid_t pid)
{
	check_fg = 0;
	pause();
	/* If check_fg is unmodified even after pause, the foreground process has not terminated yet. Hence, it must pause again until the foreground process terminates. */
	if(!check_fg)
		pause();

	return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminaates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
	pid_t pid;
	check_fg = 0;
	int jid; /* job id of the job being considered */
	int status; 
	/* status contains information about the status of the job that is stopped or terminated */
	
	/*
		Here, waitpid will check if any child process (due to -1 argument) is terminated (due to WNOHANG) or stopped (due to WUNTRACED) without pausing the parent process and will reap all its child processes.
		
		status contains information about the termination or stopping of the process which can be accessed using WIFEXITED, WIFSTOPPED, WIFSIGNALED, etc.
	*/
	while((pid = waitpid(-1,&status,WNOHANG|WUNTRACED)) > 0) {
		jid = pid2jid(pid); /* obtain jid of the job being considered from pid */
		if(jobs[jid-1].state == FG)
			check_fg = 1;
			
		/* 	
			WIFEXITED checks if the job has terminated normally after execution.
		   The job is then deleted from the joblist.
		 */
		if(WIFEXITED(status)) {
			deletejob(jobs,pid);
		}
		
		/*
			WIFSIGNALED checks if the job terminated on receiving a signal.
			The job is then deleted from the joblist.
		*/
		else if(WIFSIGNALED(status)) {
			if(WTERMSIG(status) == SIGINT) {
				deletejob(jobs,pid);
				printf("job [%d] (%d) terminated by signal %d\n",jid,pid,SIGINT);
			}
		}
		
		/*
			WIFSTOPPED checks if the job is stopped on receiving a signal.
			The state of the job is then changed to ST(i.e.stopped).
		*/
		else if(WIFSTOPPED(status)) {
			getjobpid(jobs,pid)->state = ST;
			printf("job [%d] (%d) stopped by signal %d\n",jid,pid,SIGTSTP);
			
		}
		
		
	}
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
/*
	The process ID of the foreground process is first determined. Then a SIGINT signal is sent to all processes in the foreground process group using (-pid) argument in the kill() function.
	
	Each child process has process ID = PID due to call to setpgid() function in eval.
*/
void sigint_handler(int sig) 
{
	pid_t pid = fgpid(jobs); /* PID of the foreground job */
	/* SIGINT is sent to all processes with group process ID = pid, i.e. processes in the foreground process group. */
	if(kill(-pid, SIGINT) < 0)
		unix_error("kill error\n"); 
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
/*
	The process ID of the foreground process is first determined. Then a SIGTSTP signal is sent to all processes in the foreground process group using (-pid) argument in the kill() function.
	
	State of all the job is changed to ST(i.e. stopped).
	
	Each child process has process ID = PID due to call to setpgid() function in eval.
*/
void sigtstp_handler(int sig) 
{
	pid_t pid = fgpid(jobs); /* PID of the foreground job */
	/* SIGTSTP is sent to all processes with group process ID = pid, i.e. processes in the foreground process group. */
	if(kill(-pid,SIGTSTP) < 0)
		unix_error("kill error\n"); 
	jobs[pid2jid(pid)-1].state = ST; /* state of job is changed to stopped(i.e.'ST')*/
	    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
\ **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
