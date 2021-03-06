#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <elf.h>
#include <arpa/inet.h>


#include "kernel_types.h"
#include "defs.h"
#include "x86_syscallent.h"
#include "x86_64_syscallent.h"
#include "protect.h"
#include "utils.h"

static union {
	struct user_regs_struct      x86_64_r;
	struct i386_user_regs_struct i386_r;
} x86_regs_union;

#define x86_64_regs x86_regs_union.x86_64_r
#define i386_regs   x86_regs_union.i386_r


static struct iovec x86_io = {
	.iov_base = &x86_regs_union
};


struct user_regs_struct saved_regs;

kernel_ulong_t u_arg[MAX_ARGS];

uint32_t *const i386_esp_ptr = &i386_regs.esp;
uint64_t *const x86_64_rsp_ptr = (uint64_t *) &x86_64_regs.rsp;

char *filepath = "/tmp/.pwn-sandbox";
char *fileprefix = "timestamp";

char protect_buf[PROTECT_BUFSIZE];
char temp_buf[PROTECT_BUFSIZE];
const static struct_sysent *syscallent;

static long
ptrace_getregset(pid_t pid);

static int
get_syscall_args();

static int
write_file(const char *filename, unsigned char *buf, size_t len);


void get_arch(pid_t pid){
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, pid, NULL, &regs);
    if (regs.cs == 0x33) {
        syscallent = x86_64_sysent;
    } else if (regs.cs == 0x23) {
        syscallent = x86_sysent;
    } else {
        //unsupported
    }
}

void print_args();

void save_reg(pid_t pid){
    ptrace(PTRACE_GETREGS, pid, NULL, &saved_regs); 
}

void set_logfile_path(char *path) {
    if(path != NULL)
        filepath = strdup(path);
}

void set_logfile_prefix(char *prefix){
    if(prefix != NULL)
        fileprefix = strdup(prefix);
}

void get_buffer(pid_t pid, char *dst, long addr, long len) {
    int i, word_len;
    long t, words, bytes, *p;
    char *c;
    if (syscallent == x86_64_sysent) {
        word_len = 8;
    } else {
        word_len = 4;
    }
    words = len/word_len;
    bytes = len%word_len;
    for(i=0;  i < words; ++i) {
        t = ptrace(PTRACE_PEEKTEXT, pid, addr + i * word_len, 0);
        if (syscallent == x86_64_sysent) {
            *(((unsigned long *)dst) + i) = t; 
        } else {
            *(((unsigned int *)dst) + i) = (unsigned int)t; 
        }
    }
    if(bytes > 0){
        t = ptrace(PTRACE_PEEKTEXT, pid, addr + words * word_len, 0);
        for(i=0; i<bytes; ++i) {
            dst[words * word_len + i] = *(((char*)&t)+i);
        }
    }
}

int get_string(pid_t pid, char *dst, long addr){
    int i, word_len, j, k=0, stop = 0;
    long t;
    char *c;
    if (syscallent == x86_64_sysent) {
        word_len = 8;
    } else {
        word_len = 4;
    }
    for(i=0;;++i) {
        t = ptrace(PTRACE_PEEKTEXT, pid, addr + i * word_len, 0);
        c = (char*)&t;
        for (j=0; j<word_len; ++j){
            if(c[j] != 0) {
                dst[k] = c[j];
                ++k;
            }  else {
                dst[k] = 0;
                stop = k;
                break;
            }
        }
        if (stop) {
            break;
        }
    }
    return stop;
}

/*
 * len = read(fd, buf, max);
 */
void dump_read(pid_t pid, long syscall, long len) {
    char tmp[16];
    int fd;
    snprintf(tmp, 16, "%ld", u_arg[0]);    
    get_buffer(pid, protect_buf, u_arg[1], len);
    write_file(tmp, protect_buf, len);
}

void dump_other(pid_t pid, long syscall) {
    char tmp[128];
    int fd;
    snprintf(temp_buf, 128, "%s()\n", syscallent[syscall].sys_name); 
    write_file("syscall", temp_buf, strlen(temp_buf));
}


/*
 * len = read(fd, buf, len);
 */
void dump_write(pid_t pid, long syscall) {
    int fd;
    char tmp[16];
    snprintf(tmp, 16, "%ld", u_arg[0]);    
    get_buffer(pid, protect_buf, u_arg[1], u_arg[2]);
    write_file(tmp, protect_buf, u_arg[2]);
}

/*
 * stat = execve(path, arg, env);
 */
void dump_execve(pid_t pid, long syscall) {
    int fd;
    char tmp[128]; 
    get_string(pid, protect_buf, u_arg[0]);
    snprintf(temp_buf, PROTECT_BUFSIZE, "execve(%s)\n", protect_buf);
    write_file("syscall", temp_buf, strlen(temp_buf));
    kill(pid, SIGKILL);
    exit(-1);
}

void dump_fork(pid_t pid, long syscall) {
    int fd;
    char tmp[128];  
    snprintf(temp_buf, PROTECT_BUFSIZE, "fork()");
    write_file("syscall", temp_buf, strlen(temp_buf));
    kill(pid, SIGKILL);
    exit(-1);
}

void dump_clone(pid_t pid, long syscall) {
    int fd;
    char tmp[128];    
    snprintf(temp_buf, PROTECT_BUFSIZE, "clone()\n");
    write_file("syscall", temp_buf, strlen(temp_buf));
    kill(pid, SIGKILL);
    exit(-1);
}


/*
 * fd = open(path, mode, priv);
 */
void dump_open(pid_t pid, long syscall) {
    get_string(pid, protect_buf, u_arg[0]);
    snprintf(temp_buf, PROTECT_BUFSIZE, "open(%s)\n", protect_buf);
    write_file("syscall", temp_buf, strlen(temp_buf));

    if(!strstr(protect_buf, "/lib") && 
            !strstr(protect_buf, "/etc") && 
	    !strstr(protect_buf, "/usr") && 
	    !strstr(protect_buf, "/dev/urandom")) {
        kill(pid, SIGKILL);
        exit(-1);
    }
    if( strstr(protect_buf, "flag")) {
        kill(pid, SIGKILL);
        exit(-1);
    }
    if( strstr(protect_buf, "/tmp")) {
        kill(pid, SIGKILL);
        exit(-1);
    }
}


void pwn_preprotect(pid_t pid, long syscall) {
    ptrace_getregset(pid);
    get_syscall_args(pid);
    //fprintf(stderr, "\033[31mpre syscall: %s \033[0m\n", syscallent[syscall].sys_name);
    //print_args();
    switch(syscallent[syscall].sen){
        case SEN_write:
            dump_write(pid, syscall);
            break;
        case SEN_execve:
            dump_execve(pid, syscall);
            break;
        case SEN_fork:
            dump_fork(pid, syscall);
            break;
        case SEN_clone:
            dump_clone(pid, syscall);
            break;
        case SEN_open:
            dump_open(pid, syscall);
            break;
    } 
    dump_other(pid, syscall);
}



void pwn_postprotect(pid_t pid, long syscall, long retval) {
    //fprintf(stderr, "\033[31mpost syscall: %s, retval: %ld\033[0m\n", syscallent[syscall].sys_name, retval);
    switch(syscallent[syscall].sen){
        case SEN_read:
            dump_read(pid, syscall, retval);
            break;
    } 
}


static int
write_file(const char *filename, unsigned char *buf, size_t len)
{
    char *path = malloc(8192);
    int cnt, fd, t;
    if(filename[0] == '0' && filename[1] == 0) {
        snprintf(path, 8192, "%s/%s-std", filepath, fileprefix);
        fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0666);
        if(fd == -1) return -1;
        cnt = write(fd, "\x00", 1);
        t = htonl((unsigned int)len);
        cnt = write(fd, ((unsigned char *)&t) + 1, 3);
        cnt = write(fd, buf, len);
    } else if (filename[0] == '1' && filename[1] == 0) {
        snprintf(path, 8192, "%s/%s-std", filepath, fileprefix);
        fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0666);
        if(fd == -1) return -1;
        cnt = write(fd, "\x01", 1);
        t = htonl((unsigned int)len);
        cnt = write(fd, ((unsigned char *)&t) + 1, 3);
        cnt = write(fd, buf, len);
    } else {
        snprintf(path, 8192, "%s/%s-%s", filepath, fileprefix, filename);
        fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0666);
        if(fd == -1) return -1;
        cnt = write(fd, buf, len);
    }
    close(fd);
    free(path);
    return cnt;
}

void print_args(){
    int i;
    fprintf(stderr, "\033[31msyscall args: ");
    for (i=0; i<6; ++i){
        fprintf(stderr, "0x%lx ", u_arg[i]);
    }
    fprintf(stderr, "\033[0m\n");
}

static long
ptrace_getregset(pid_t pid)
{
# ifdef ARCH_IOVEC_FOR_GETREGSET
	/* variable iovec */
	ARCH_IOVEC_FOR_GETREGSET.iov_len = sizeof(ARCH_REGS_FOR_GETREGSET);
	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS,
		      &ARCH_IOVEC_FOR_GETREGSET);
# else
	/* constant iovec */
	static struct iovec io = {
		.iov_base = &ARCH_REGS_FOR_GETREGSET,
		.iov_len = sizeof(ARCH_REGS_FOR_GETREGSET)
	};
	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io);

# endif
}

/*
 * PTRACE_GETREGSET was added to the kernel in v2.6.25,
 * a PTRACE_GETREGS based fallback is provided for old kernels.
 */
static int
getregs_old(pid_t pid)
{
	/* Use old method, with unreliable heuristical detection of 32-bitness. */
	long r = ptrace(PTRACE_GETREGS, pid, NULL, &x86_64_regs);
	if (r)
		return r;

	if (x86_64_regs.cs == 0x23) {
		/*
		 * The order is important: i386_regs and x86_64_regs
		 * are overlaid in memory!
		 */
		i386_regs.ebx = x86_64_regs.rbx;
		i386_regs.ecx = x86_64_regs.rcx;
		i386_regs.edx = x86_64_regs.rdx;
		i386_regs.esi = x86_64_regs.rsi;
		i386_regs.edi = x86_64_regs.rdi;
		i386_regs.ebp = x86_64_regs.rbp;
		i386_regs.eax = x86_64_regs.rax;
		/* i386_regs.xds = x86_64_regs.ds; unused by strace */
		/* i386_regs.xes = x86_64_regs.es; ditto... */
		/* i386_regs.xfs = x86_64_regs.fs; */
		/* i386_regs.xgs = x86_64_regs.gs; */
		i386_regs.orig_eax = x86_64_regs.orig_rax;
		i386_regs.eip = x86_64_regs.rip;
		/* i386_regs.xcs = x86_64_regs.cs; */
		/* i386_regs.eflags = x86_64_regs.eflags; */
		i386_regs.esp = x86_64_regs.rsp;
		/* i386_regs.xss = x86_64_regs.ss; */
	}
	return 0;
}

/* Return -1 on error or 1 on success (never 0!). */
static int
get_syscall_args()
{
	if (syscallent == x86_64_sysent) {
        u_arg[0] = x86_64_regs.rdi;
        u_arg[1] = x86_64_regs.rsi;
        u_arg[2] = x86_64_regs.rdx;
        u_arg[3] = x86_64_regs.r10;
        u_arg[4] = x86_64_regs.r8;
        u_arg[5] = x86_64_regs.r9;
	} else {
		/*
		 * i386 ABI: zero-extend from 32 bits.
		 * Use truncate_klong_to_current_wordsize(tcp->u_arg[N])
		 * in syscall handlers
		 * if you need to use *sign-extended* parameter.
		 */
		u_arg[0] = (uint32_t) i386_regs.ebx;
		u_arg[1] = (uint32_t) i386_regs.ecx;
		u_arg[2] = (uint32_t) i386_regs.edx;
		u_arg[3] = (uint32_t) i386_regs.esi;
		u_arg[4] = (uint32_t) i386_regs.edi;
		u_arg[5] = (uint32_t) i386_regs.ebp;
	}
	return 1;
}
