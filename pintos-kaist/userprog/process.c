#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif


#define ARGUMENT_LIMIT 64 // 명령행으로 받을 인자의 최댓값
#define STACK_LIMIT (USER_STACK - PGSIZE)
// commit test

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
static bool push_stack_fr(struct intr_frame *if_);


/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *save_ptr;

	// file_name ="args-single onearg"
	char* prog_name = strtok_r(file_name, " ", &save_ptr);
	// file_name ="args-single"
	// fn_copy = "args-single onearg"
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page(fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();
	// f_name = "args-single onearg"
	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

// [*]2-B. fork 시 관리 위한 구조체
struct fork_args
{
	struct thread *parent;	// 자식이 자신의 부모를 알 수 있게
	struct intr_frame *if_; // 부모의 레지스터 상태를 복제할 수 있게
	struct child_info *ci;	// 자식이 자기 child_info를 알 수 있게
};

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
// [*]2-B. fork 구현
tid_t process_fork(const char *name, struct intr_frame *if_)
{
	/* Clone current thread to new thread.*/
	// return thread_create (name,
	// 		PRI_DEFAULT, __do_fork, thread_current ());

	// 1. 부모가 자식의 종료 상태를 수거할 구조체를 만듦
	struct child_info *ci = palloc_get_page(sizeof(struct child_info));
	if (ci == NULL)
		return TID_ERROR;

	ci->waited = false;
	ci->exit_status = -1;
	sema_init(&ci->wait_sema, 0);

	// 2. 자식에게 넘겨줄 인자 패키지를 구성
	struct fork_args *args = palloc_get_page(sizeof(struct fork_args));
	if (args == NULL)
	{
		palloc_free_page(ci);
		return TID_ERROR;
	}
	args->parent = thread_current();
	args->if_ = if_;
	args->ci = ci;

	// 3. 자식 스레드를 생성, aux 인자로 args를 넘김
	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, args->parent);
	if (tid == TID_ERROR)
	{
		palloc_free_page(ci);
		palloc_free_page(args);
		return TID_ERROR;
	}

	// 4. 자식 구조체 tid 세팅 후 부모의 리스트에 추가
	ci->tid = tid;
	list_push_back(&thread_current()->child_list, &ci->elem);

	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	// 커널 페이지인지 검사

	/* 2. Resolve VA from the parent's page map level 4. */
	// 부모의 VA로부터 실제 물리 페이지 가져오기
	parent_page = pml4_get_page(parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	// 자식용 새 페이지 할당

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	// 부모 → 자식 페이지 복사
	// writable 여부 판단

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	// 자식 page table에 insert

	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		// 실패 시 palloc_free_page() 하고 false 리턴
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
// 부모 프로세스의 실행 컨텍스트(CPU 상태)를 복사하는 자식 스레드의 시작점 함수
// parent->tf는 유저 영역의 정확한 레지스터 상태를 가지고 있지 않으므로,
// process_fork()에서 받은 intr_frame 복사본을 두 번째 인자로 받아서 사용해야 한다.
static void
__do_fork(void *aux)
{
	// struct intr_frame if_;
	// struct thread *parent = (struct thread *)aux;
	// struct thread *current = thread_current();
	// /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	// // fork 시 복제할 부모의 레지스터 상태가 필요
	// struct intr_frame *parent_if;
	bool succ = true;

	// /* 1. Read the cpu context to local stack. */
	// // 부모로부터 받은 유저 실행 상태(struct intr_frame)를 자식의 지역 변수에 복사한다.
	// // 이게 있어야 나중에 do_iret(&if_)로 유저 모드에 진입 가능.
	// memcpy(&if_, parent_if, sizeof(struct intr_frame));

	// [*]2-B. 자식 쪽 (__do_fork()) 코드도 맞춰주기
	// [*]2-B. 자식은 aux로 넘긴 struct fork_args *를 받아서 self_child_info, parent를 설정
	struct fork_args *args = (struct fork_args *)aux;
	struct thread *current = thread_current();
	current->parent = args->parent; // 부모-자식 연결
	current->self_child_info = args->ci;
	struct intr_frame *parent_if = args->if_; // 레지스터 상태 복사
	struct intr_frame if_;
	memcpy(&if_, parent_if, sizeof(struct intr_frame));
	// void *memcpy(void *dest, const void *src, size_t n)
	// src 주소로부터 n바이트를 읽어서 dest 주소로 복사한다.
	palloc_free_page(args); // 더 이상 필요 없는 인자는 해제

	/* 2. Duplicate PT */
	// 부모의 페이지 테이블을 자식에게 복사하는 과정.
	// VM이 활성화되어 있다면 supplemental page table(SPT)를 복사하고,
	// 아니라면 pml4_for_each()로 PTE를 복제함
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else // 부모의 사용자 주소 공간을 자식에게 복사하는 과정 - VM을 사용하지 않는 경우
	if (!pml4_for_each(args->parent->pml4, duplicate_pte, args->parent))
		// 부모의 페이지 테이블(pml4)을 하나씩 순회하며, 각각의 유저 페이지(va, pte)를 duplicate_pte()에 넘기는 구조
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	// 힌트: 부모의 열려있는 파일들을 복제할 때는 file_duplicate()를 사용하라.
	// 중요한 점은, 부모는 자식이 모든 자원 복제에 성공했을 때에만 fork()에서 리턴해야 한다.
	// (즉, 자식이 준비되기도 전에 부모가 진행되면 안 된다.)

	process_init();

	/* Finally, switch to the newly created process. */
	// 자식 프로세스의 준비가 끝났다면, 실제 유저모드로 진입 (do_iret) 시도한다.
	if (succ)
		do_iret(&if_);
error:
	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
// 현재 프로세스를 새로운 실행파일로 덮어쓰기 위한 함수
// [*]2-O 문자열 파싱, 스택프레임 구성
int process_exec(void *f_name)
{
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;


	/* We first kill the current context */
	// 부모-자식 관계 상에서 자식 프로세스가 “새로운 실행 파일을 불러오기 전에” ,
	// 기존 환경을 청소하는 작업.
	process_cleanup();

	/* [*]
	for implement argument passing
	before load,
	스택 프레임에 프로그램 실행을 위한 정보들(인자 문자열, argv 배열, argc, fake return address 등)을
	쌓아넣기 위해 받은 입력값을 파싱하는 작업을 이 위치에서 수행합니다.
	
	유저 애플리케이션은 인자 전달을 위해 %rdi, %rsi, %rdx, %rcx, %r8, %r9 순서로 정수 레지스터를 사용함.

	
	공백을 기준으로 문자열을 나눠서,
	첫 번째 단어는 프로그램 이름
	두번째 단어부터 첫번째 인자로 처리되도록 구현
	*/
	// 

	int argc = 0;
	char *argv[ARGUMENT_LIMIT];
	char *token, *save_ptr;

	// 현재 file_name = "args-single onearg" 

	token = strtok_r(file_name, " ", &save_ptr);
	// 모든 토큰을 처리
	while (token != NULL && argc < ARGUMENT_LIMIT)
	{
		// 현재 토큰을 argv 배열에 저장
		argv[argc] = token;
		argc++;

		// 다음 토큰 가져오기
		token = strtok_r(NULL, " ", &save_ptr);
	}

	if (argc < ARGUMENT_LIMIT)
	{
		argv[argc] = NULL;
	}

	/* 
	파싱 후
	argc = 2

	argv[0] = "args-single"
	argv[1] = "onearg" 
	argv[2] = NULL
	*/

	file_name = argv[0];
	// 레지스터에 main함수에서 쓰이는 첫번째 인자와 두번째 인자 전달.
	_if.R.rdi = argc;
	_if.R.rsi = (uint64_t) argv; // 주소값을 정수로 전달할 때, uint64_t를 사용. 

	/* And then load the binary */
	success = load(file_name, &_if);

	thread_current()->load_success = success; //[*]2-B. exec 성공 여부 부모에게 전달

	// [*]2-B. !!! load_success 저장 후 sema_up()로 부모 깨워야함
	// -> start_process()에서 sema_up(&thread_current()->load_sema); 함수가 없나??
	push_stack_fr(&_if);
	// 레지스터에 main함수에서 쓰이는 첫번째 인자와 두번째 인자 전달.
	// 주소값을 정수로 전달할 때, uint64_t를 사용. 
	//hex_dump(_if.rsp, _if.rsp, USER_STACK - (uint64_t)_if.rsp, true);

	/* If load failed, quit. */
	palloc_free_page(file_name);
	if (!success)
		return -1;

	// 여기부터 유저 영역
	/* Start switched process. */

	// 유저 영역에 들어가면서 시스템 콜을 호출할텐데,
	// 커널에선 시스템 콜 번호와 인자를 확인한 후
	// 그에 맞는 시스템 콜 핸들러 함수가 호출되고
	// 그 핸들러가 요청을 적당히 처리하고(출력, 프로세스 관리 등) ㄱ결과를 사용자 프로그램에 반환한 뒤 사용자 모드로 복귀

	// printf("before do_iret\n");
	do_iret(&_if);
	// do_iret가 호출된 이후로부턴 syscall.c에 구현된 syscall handler가 역할을 함.
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */

int process_wait(tid_t child_tid) //UNUSED 지움
{
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */

	// [*]2-B. 대기
	struct thread *cur = thread_current();
	struct list_elem *e;
	for (e = list_begin(&cur->child_list); e != list_end(&cur->child_list); e = list_next(e)) // 자식 리스트를 순회
	{
		struct child_info *child = list_entry(e, struct child_info, elem); // 리스트에서 child_info 구조체 추출
		if (child->tid != child_tid)									   // child_tid가 일치하는 자식만 wait
			continue;
		if (child->waited) // 이미 wait() 호출된 자식이라면 중복 호출 → -1 반환
			return -1;
		child->waited = true;			 // 처음 wait() 호출하는 경우 → waited = true 표시
		sema_down(&child->wait_sema);	 // 자식이 종료될 때까지 대기 (sema_down)
		int status = child->exit_status; // 자식이 종료된 후 exit_status를 받아옴
		list_remove(e);					 // 자식 정보를 리스트에서 제거하고
		palloc_free_page(child);		 // 메모리 해제
		return status;					 // 자식 종료 상태를 반환
	}

	return -1; // 자식 리스트에서 해당 pid를 찾지 못했거나 조건 미충족 시 -1 반환
// 	for (int i=0; i < 1000000000; i++){
// 	}
// 	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	struct thread *cur = thread_current(); // 현재 종료 중인 스레드

	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	// [*]2-B. 종료
	if (cur->self_child_info != NULL) // 자식이면서, 부모가 나를 wait()할 수 있게 등록해둔 상태인 경우
	{
		cur->self_child_info->exit_status = cur->exit_status; // 내 종료 상태를 부모에게 전달
		sema_up(&cur->self_child_info->wait_sema);			  // 부모가 sema_down()으로 기다리고 있었으므로 깨워줌
	}
	process_cleanup(); // 그 외 자원 정리 (page table, 파일 디스크립터 등)

	// // [*]2-B. !!!
	// syscall_handler() 내부에서 case SYS_EXIT 따지고  sys_exit(args[1]); 접근하는 곳에 추가
	// void sys_exit(int status) {
	// thread_current()->exit_status = status;
	// thread_exit();
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */

/*
디스크의 실행 파일을 메모리에 올려서 CPU가 바로 실행할 수 있는 상태로 만드는 것
가상 메모리, 페이지 테이블, 스택 세팅 등 실행 환경 전체를 준비하는 과정을 포함
*/
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	// 가상주소공간 준비
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());

	/* Open executable file. */
	file = filesys_open(file_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	// 헤더 검증
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	// 프로그램 카운터
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	//if_->

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	file_close(file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

// [*]2-O 스택 프레임 구성용 함수
static bool push_stack_fr(struct intr_frame *if_){
	char** argv = (char **) if_->R.rsi;
	int argc = if_->R.rdi;
	// 스택에 저장된 문자열의 주소를 저장 추후 프레임에 추가
	char *addrs_argv[argc]; 


	// argv 문자열 먼저 푸쉬
	for (int i = argc-1; i >=0; i--){
		size_t len = strlen(argv[i]) + 1; // 널 종단문자 포함
		if_->rsp -= len;
		if ((uint64_t)if_->rsp < STACK_LIMIT)
			return false;
		memcpy(if_->rsp, argv[i], len);
		addrs_argv[i] = if_->rsp;
	}

	// 정렬용 패딩
	if_->rsp = (uint64_t)if_->rsp & ~ 0x7;

	// 문자열 시작주소 푸쉬
	// 마지막 문자열 표시
	if_->rsp -= sizeof(uintptr_t);
	memset(if_->rsp, 0, sizeof(uintptr_t));

	for (int i = argc -1; i>=0; i--){
		if_->rsp -= sizeof(uintptr_t);
		memcpy(if_->rsp, &addrs_argv[i], sizeof(uintptr_t));
	}

	if_->R.rsi = if_->rsp;
	// 규약상 필요한 주소에 가짜주소 채워넣기
	if_->rsp -= sizeof(uintptr_t);
	memset(if_->rsp, 0, sizeof(uintptr_t));

	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
