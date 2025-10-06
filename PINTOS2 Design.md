# PINTOS 1 Design Report

Team12 한라봉 / 컴퓨터공학과 20210807 이세광 컴퓨터공학과 20210750 이명률

---

## 1. Analysis of process execution procedure

### threads/init.c

이 파일은 Pintos 커널의 시작점이다. 컴퓨터가 부팅되고 Pintos 커널이 메모리에 로드되면 가장 먼저 `main()` 함수가 실행된다. `main()` 함수는 운영체제에 필요한 모든 핵심 요소들(메모리 관리, 쓰레드, 인터럽트 등)을 순서대로 초기화하고, 마지막으로 사용자가 실행하라고 명령한 프로그램을 실행시키는 역할을 한다.

---

`main()`: 시스템 부팅 및 초기화

+ 초기화 단계: main 함수는 BSS 세그먼트 초기화, 메모리 관리자(palloc, malloc, paging) 초기화, 쓰레드 시스템(thread_init) 초기화, 인터럽트 핸들러(intr_init) 및 각종 장치 드라이버 초기화를 수행한다.

+ `#ifdef USERPROG`: Project 2(userprog)부터 활성화되는 부분. 사용자 프로세스를 커널과 분리하기 위한 TSS와 GDT를 설정하고, 시스템 콜과 예외 처리를 위한 `syscall_init()`, `exception_init()`을 호출한다.

+ 스케줄러 시작: `thread_start()` 함수를 통해 쓰레드 스케줄러가 동작을 시작하고 멀티태스킹이 가능해진다.

+ `run_actions(argv)` 호출: 모든 시스템 초기화가 완료된 후, main 함수는 커맨드 라인에서 받은 인자(argv)를 `run_actions` 함수에 넘겨주어 실제 작업을 수행하도록 한다. 프로세스 실행은 이 함수로부터 시작된다.


`run_actions()`: 커맨드 라인 작업 실행.

pintos 명령어와 함께 주어진 작업(action)을 찾아 실행한다. 예를 들어 pintos -- -q run alarm-multiple 이라는 명령어가 있다면, run이 action에 해당한다.

`run_task()`: 특정 작업(프로세스) 실행 요청. run action에 대한 실질적인 처리를 담당하며, 가장 핵심적인 부분은 `process_wait (process_execute (task));`이다. Pintos 2(USERPROG)부터 이 코드가 실행된다.

### userprog/process.c

`process_execute()`: 사용자 프로그램을 실행하기 위한 새로운 쓰레드를 생성함.

`start_process()`: `process_execute`에 의해 생성된 새로운 커널 쓰레드는 이 함수에서 실행을 시작한다. 이 함수의 최종 목표는 커널 모드에서 사용자 모드로 전환하여 사용자 프로그램을 실행시키는 것이다.

`load()`: LF(Executable and Linkable Format) 형식의 실행 파일을 파싱하고, 파일의 내용을 가상 메모리에 올리는 복잡한 작업을 수행한다.

### How process execution works in the current Pintos system?

#### 1. 커널 부팅 및 실행 준비 (`init.c`)

1.  `main()` 함수 실행: Pintos 커널이 시작되면 `threads/init.c`의 `main()` 함수가 가장 먼저 실행된다.
2.  핵심 시스템 초기화: `main()` 함수는 메모리 관리 시스템(`palloc_init`, `paging_init`), 쓰레드 시스템(`thread_init`), 인터럽트 핸들러(`intr_init`) 등 커널의 핵심 기능들을 초기화한다.
3.  사용자 프로그램 지원 기능 초기화: `#ifdef USERPROG` 블록 안에서 사용자 프로세스를 지원하기 위한 GDT(`gdt_init`), TSS(`tss_init`), 시스템 콜 핸들러(`syscall_init`) 등을 초기화한다.
4.  커맨드 라인 파싱: `read_command_line()`과 `parse_options()`를 통해 사용자가 Pintos를 실행할 때 입력한 명령어를 분석한다.
5.  실행 명령 전달: 모든 초기화가 끝나면, `main()` 함수는 `run_actions()`를 호출한다. `run_actions()`는 `run`이라는 명령어를 발견하고, 이에 매핑된 `run_task()` 함수를 호출한다.

#### 2. 프로세스 생성 요청 (`init.c` -\> `process.c`)

이 단계는 `init.c`에서 `process.c`로 책임이 넘어가는 연결 지점.

1.  `run_task()` 실행: `init.c`의 `run_task()` 함수는 실행할 프로그램의 이름을 인자로 받는다.
2.  `process_execute()` 호출: `run_task()`는 `userprog/process.c`에 정의된 `process_execute()` 함수를 호출하여 실질적인 프로세스 생성을 요청한다.
    ```c
    // in threads/init.c, run_task()
    process_wait (process_execute (task));
    ```
    이 한 줄이 모든 프로세스 실행의 시작점입니다.

#### 3. 새로운 쓰레드 생성 및 프로그램 로드 준비 (`process.c`)

1.  `process_execute()`:

      - `file_name`의 복사본을 만들어 경쟁 상태를 방지한다.
      - `thread_create()`를 호출하여 **새로운 커널 쓰레드**를 생성한다. 이 쓰레드는 사용자 프로그램을 실행할 주체가 된다.
      - 이때, 새로 생성된 쓰레드가 실행을 시작할 함수로 `start_process`를 지정하고, 인자로는 프로그램 이름 복사본을 넘겨준다.
      - `process_execute`는 즉시 새로 생성된 쓰레드의 ID(`tid`)를 반환한다.

2.  `start_process()`:

      - 새로 생성된 커널 쓰레드는 이 함수에서 실행을 시작한다.
      - 가장 먼저 하는 일은 `load()` 함수를 호출하여 디스크에 있는 실행 파일을 메모리에 올리는 것이다.
      - `load()` 함수가 성공적으로 완료되면, 프로그램의 시작 주소(`eip`)와 사용자 스택 포인터(`esp`) 값을 얻게 된다.

#### 4. ELF 파일 로드 및 가상 메모리 설정 (`process.c`의 `load` 함수)

`load()` 함수는 프로세스 실행의 가장 기술적인 부분을 담당.

1. *페이지 디렉토리 생성: 각 프로세스는 독립적인 가상 주소 공간을 가진다. `pagedir_create()`를 호출하여 이 프로세스만을 위한 새로운 페이지 디렉토리를 생성하고 활성화한다.
2.  ELF 파일 파싱: 실행 파일을 열고, 이것이 유효한 ELF(Executable and Linkable Format) 바이너리인지 헤더 정보를 읽어 검증한다.
3.  세그먼트 적재: ELF 파일의 프로그램 헤더를 읽어 메모리에 적재해야 할 세그먼트(Code, Data 등)를 확인한다.
4.  메모리 할당 및 매핑: `load_segment()`를 통해 각 세그먼트의 크기만큼 물리 메모리 페이지(`palloc_get_page`)를 할당받는다. 그 후 파일에서 내용을 읽어 채우고, 이 물리 페이지를 프로세스의 가상 주소 공간에 매핑(`install_page`)한다.
5.  스택 생성: `setup_stack()`을 통해 사용자 스택으로 사용할 물리 메모리 페이지 한 개를 할당하고, 가상 주소 공간의 최상단에 매핑한다.

#### 5. 사용자 모드로 전환 및 프로그램 실행 (`process.c`)

1.  실행 정보 준비: `load()` 함수가 성공적으로 리턴하면, `start_process` 함수는 프로그램의 시작 주소(`eip`)와 스택 포인터(`esp`)를 `intr_frame` 구조체에 저장한다.
2.  컨텍스트 스위칭 시뮬레이션: `start_process`의 마지막 부분에 있는 인라인 어셈블리 코드가 실행된다.
    ```c
    // in userprog/process.c, start_process()
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    ```
      - 이 코드는 인터럽트에서 복귀하는 과정을 흉내 낸다. 스택 포인터가 `intr_frame`을 가리키게 한 후 `intr_exit`으로 점프하면, `iret` 명령이 실행되면서 CPU는 **커널 모드에서 사용자 모드로 전환**된다.
      - 이때 `intr_frame`에 저장해두었던 값들이 CPU 레지스터로 복원되면서, Instruction Pointer(`EIP`)는 프로그램의 시작 주소를, Stack Pointer(`ESP`)는 사용자 스택을 가리키게 된다.
3.  사용자 프로그램 실행 시작: 마침내 CPU는 사용자 프로그램의 첫 번째 기계어 명령부터 실행을 시작한다.

#### 6. 부모 프로세스의 대기 및 시스템 종료 (`init.c`)

  - 자식 프로세스가 위 3\~5단계를 거쳐 실행되는 동안, 부모 프로세스(최초의 `main` 쓰레드)는 2단계에서 호출했던 `process_wait()` 함수 안에서 자식 프로세스가 종료될 때까지 대기한다.
  - 사용자 프로그램 실행이 모두 끝나고 `exit` 시스템 콜을 호출하면, 자식 프로세스는 `process_exit()`를 통해 모든 자원을 해제하고 종료된다.
  - 이때 `process_wait()`에서 대기하던 부모가 깨어나고, `run_task()` 함수가 리턴된다.
  - 모든 `run` action이 끝나면 `main` 함수는 `shutdown()`을 호출하여 Pintos를 종료한다.

#### 요약

`init.c: main()` -\> `init.c: run_actions()` -\> `init.c: run_task()`
**-- 경계 --**
\-\> `process.c: process_execute()` (새 쓰레드 생성)
\-\> `(새 쓰레드)` -\> `process.c: start_process()`
\-\> `process.c: load()` (ELF 파일 로드 및 메모리 매핑)
\-\> `process.c: start_process()` (사용자 모드로 전환)
**-\> [사용자 프로그램 실행]**
**-\> [프로그램 종료]** -\> `process.c: process_exit()`
**-- 경계 --**
\-\> `init.c: process_wait()` 리턴 -\> `init.c: shutdown()`

## 2. Analysis of system call procedure

### lib/user/syscall.c

이 파일은 사용자 프로그램(User Program)의 영역에서 동작하는 코드이다. 즉, 커널 코드가 아니라, 사용자 프로그램이 커널의 기능을 사용하기 위해 호출하는 C 표준 라이브러리 함수들의 Pintos 구현체이다.

사용자 프로그램은 파일 열기, 화면에 글자 출력, 프로세스 종료 등 운영체제의 보호된 기능이 필요할 때 직접 커널 코드에 접근할 수 없습니다. 대신, 이 파일에 정의된 함수들을 통해 커널에게 서비스를 요청하게 된다.

**핵심 메커니즘: syscall 매크로와 인라인 어셈블리**

이 파일의 모든 함수들(halt, exit, write 등)은 내부적으로 `syscall0`부터 `syscall3`까지의 매크로 중 하나를 호출한다. 이 매크로들이 바로 사용자 모드에서 커널 모드로 전환하는 핵심적인 역할을 수행한다.

```c
#define syscall1(NUMBER, ARG0)                                              \
        ({                                                                  \
          int retval;                                                       \
          asm volatile                                                      \
            ("pushl %[arg0]; pushl %[number]; int $0x30; addl $8, %%esp" \
                : "=a" (retval)                                             \
                : [number] "i" (NUMBER),                                    \
                  [arg0] "g" (ARG0)                                         \
                : "memory");                                                \
          retval;                                                           \
        })
```

이 매크로는 GCC 인라인 어셈블리(`asm volatile`)를 사용하여 저수준의 CPU 명령을 직접 실행한다. `int $0x30` 명령어가 가장 중요하다. **소프트웨어 인터럽트(Software Interrupt)** 0x30번을 발생시킨다. 이 명령이 실행되는 순간, CPU는 현재 실행 중인 사용자 프로그램을 **일시 중단**하고, 미리 등록된 커널의 인터럽트 핸들러로 제어권을 넘긴다. 즉, **사용자 모드에서 커널 모드로 전환**되는 결정적인 지점이다.

Wrapper 함수들: exit, write 등

파일의 나머지 함수들은 이 syscall 매크로들을 감싸는 간단한 Wrapper 함수이다.

### threads/intr-stubs.S

int $0x30 명령어가 실행된 직후에 CPU가 가장 먼저 실행하는 커널 코드. 모든 종류의 인터럽트, 예외, 그리고 시스템 콜이 커널로 들어오는 공통된 진입로 역할을 한다.

**인터럽트 처리 과정**

1. int $0x30 명령이 실행되면 CPU는 하던 일을 멈춘다.

2. CPU는 **IDT(Interrupt Descriptor Table)**라는 커널이 미리 설정해 둔 테이블을 참조한다.

3. IDT의 0x30번 항목에 등록된 코드 주소로 점프하여 실행을 시작합니다. Pintos에서는 이 주소가 바로 이 파일에 정의된 intr30_stub을 가리킨다.

---

인터럽트 스텁 (intrNN_stub): 이 파일의 가장 아랫부분에는 STUB 매크로를 이용해 256개의 작은 코드 조각(intr00_stub ~ intrff_stub)을 자동으로 생성하는 부분이 있다.

```
/* Emits a stub for interrupt vector NUMBER. */
#define STUB(NUMBER, TYPE)                      \
    .text;                                      \
.func intr##NUMBER##_stub;                      \
intr##NUMBER##_stub:                            \
    TYPE;                                       \
    push $0x##NUMBER;                           \
    jmp intr_entry;                             \
.endfunc;                                       \
/* ... */

/* ... */
STUB(30, zero) STUB(31, zero) /* ... */
```

syscall.c에서 int $0x30을 호출했으므로, CPU는 IDT를 통해 intr30_stub으로 점프한다.

---

intr_entry: C 핸들러 호출 준비

모든 intrNN_stub은 `intr_entry`로 모인다. 이 부분은 인터럽트를 처리할 C 함수(intr_handler)를 호출하기 전에 필요한 모든 준비 작업을 수행한다.

intr_exit: 사용자 프로그램으로의 복귀

intr_handler의 실행이 끝나면 intr_exit으로 돌아와 사용자 프로그램으로 복귀할 준비를 한다.


### threads/interrupt.c

`intr-stubs.S`에서 넘어온 제어권을 받아, 어떤 인터럽트가 발생했는지 C언어 수준에서 판단하고, 그에 맞는 **실질적인 처리 함수(handler)를 호출해주는 '교통 경찰'**과 같은 역할을 한다. `intr-stubs.S`가 모든 인터럽트의 저수준 진입로였다면, `interrupt.c`는 그 진입로를 통과한 인터럽트들을 분류하고 분배하는 고수준의 관리 센터이다.

1. `intr_init()`: 인터럽트 시스템 초기화 (부팅 시 1회 실행)

이 함수는 커널이 부팅될 때 init.c의 main() 함수에 의해 호출된다. 인터럽트 처리에 필요한 모든 기반 시설을 설정한다.

2. `intr_register_int()`: 특정 인터럽트 핸들러 등록
이 함수는 특정 인터럽트 번호에 대해 '이 인터럽트가 발생하면 이 C 함수를 호출해줘'라고 커널에 공식적으로 등록하는 역할을 한다. 다른 커널 모듈(타이머, 키보드, 그리고 시스템 콜 핸들러)이 이 함수를 사용하여 자신의 처리 함수를 등록한다.

3. `intr_handler()`: 인터럽트 분배 (Dispatcher)
이 함수는 intr-stubs.S의 intr_entry에서 call 명령을 통해 직접 호출되는 C 함수입니다. 모든 인터럽트가 거쳐 가는 중앙 처리 지점이다.

### How a user program calls syscall_handler( ) in pintos/src/userprog/syscall.c?

Pintos에서 시스템 콜은 사용자 프로그램이 커널의 보호된 기능을 안전하게 요청하기 위한 공식적인 절차이다. 이 과정은 사용자 모드에서 커널 모드로의 정교한 제어권 이양을 포함하며, 다음과 같은 단계로 이루어진다.

#### 1. 사용자 라이브러리 함수 호출

모든 것은 사용자 프로그램이 평범한 C 함수를 호출하는 것에서 시작한다. 예를 들어, 화면에 "hello"를 출력하기 위해 다음과 같은 코드를 실행한다

```c
write(1, "hello", 5); // fd=1(stdout), "hello", 5 bytes
```

1.  Wrapper 함수 호출: 이 `write()` 함수는 커널 코드가 아닌 `lib/user/syscall.c`에 정의된 사용자 라이브러리 함수이다.
2.  `syscall` 매크로 실행: `write()` 함수는 내부적으로 인자 3개를 받는 `syscall3` 매크로를 호출한다.
    ```c
    // in lib/user/syscall.c
    int write (int fd, const void *buffer, unsigned size)
    {
      return syscall3 (SYS_WRITE, fd, buffer, size);
    }
    ```

-----

#### 2. 커널 모드 전환 준비 및 실행 (`lib/user/syscall.c`의 어셈블리)

`syscall3` 매크로는 인라인 어셈블리를 통해 커널을 호출하기 위한 모든 준비를 한다.

```assembly
; syscall3 매크로가 생성하는 어셈블리 코드
pushl %[arg2]        ; 3번째 인자(size)를 스택에 push
pushl %[arg1]        ; 2번째 인자(buffer)를 스택에 push
pushl %[arg0]        ; 1번째 인자(fd)를 스택에 push
pushl %[number]      ; 시스템 콜 번호(SYS_WRITE)를 스택에 push
int $0x30            ; 커널을 호출!
addl $16, %%esp      ; 스택 정리 (4 args * 4 bytes)
```

1.  인자 전달: Pintos 매뉴얼에서 언급한 "80x86 Calling Convention"에 따라, 시스템 콜에 필요한 모든 인자와 시스템 콜 번호가 사용자 스택에 push됩니다. 인자는 역순으로, 시스템 콜 번호가 가장 마지막에 push된다.
2.  소프트웨어 인터럽트 발생: **`int $0x30`** 명령어가 실행됩니다. 매뉴얼에서 설명하듯, 이 명령어는 CPU에게 0x30번 소프트웨어 예외(Exception)를 발생시킵니다. 이 순간, CPU는 하던 일을 멈추고 사용자 모드에서 커널 모드로 전환한 뒤, 커널이 미리 지정해둔 코드를 실행하기 시작한다.

-----

#### 3. 인터럽트 저수준 처리 

CPU는 `int $0x30` 명령을 받으면, 커널이 부팅 시 `interrupt.c`의 `intr_init()`에서 설정해 둔 IDT(Interrupt Descriptor Table)를 참조하여 0x30번 항목에 등록된 `intr30_stub`으로 점프한다.

1. `intr30_stub` 실행: 이 스텁은 스택에 더미 에러 코드(0)와 인터럽트 번호(0x30)를 push한다.
2. `intr_entry`로 점프: 모든 스텁의 공통 처리 지점인 `intr_entry`로 이동한다.
3.  컨텍스트 저장: `intr_entry`에서는 `pushal` 등의 명령을 통해 사용자 프로그램의 모든 레지스터 상태(EAX, EBX 등)를 커널 스택에 저장한다. 이 저장된 정보 덩어리가 C에서 다루게 될 `struct intr_frame`이다.
4. `intr_handler` 호출: 모든 상태를 저장한 후, 어셈블리 코드는 `call intr_handler`를 통해 `threads/interrupt.c`에 있는 C 함수를 호출한다.

-----

#### 4. 인터럽트 분배 및 최종 핸들러 호출 

1.  `intr_handler()` 실행: `intr-stubs.S`는 이 함수를 호출하면서 인자로 커널 스택에 저장된 `struct intr_frame`의 포인터를 넘겨준다.
2.  핸들러 조회: `intr_handler`는 `intr_frame`에서 인터럽트 번호 `vec_no`를 확인한다. 이 값은 **0x30**이다.
    ```c
    // in threads/interrupt.c
    handler = intr_handlers[frame->vec_no];
    ```
3.  최종 목적지 도착: `intr_handler`는 `intr_handlers` 배열의 0x30번 인덱스에 저장된 함수 포인터를 가져온다. 이 포인터는 부팅 시 `syscall_init()`에 의해 `syscall_handler` 함수를 가리키도록 미리 설정되었다.
4.  `syscall_handler()` 호출: `handler(frame);` 코드가 실행되면서, 드디어 `userprog/syscall.c`의 `syscall_handler()` 함수가 호출됩니다.

-----

#### 요약: `syscall_handler()` 호출까지의 경로

**사용자 공간 (User Space)**

1.  **C 라이브러리 함수 호출**: `write(1, "hello", 5);`
2.  **`syscall` 매크로 (`lib/user/syscall.c`)**: 인자와 시스템 콜 번호를 **사용자 스택**에 `push`.
3.  **`int $0x30` 실행**: 커널 모드로의 전환을 요청.

**경계 (Mode Switch)**
4.  **CPU 트랩**: CPU가 사용자 모드를 중단하고 커널 모드로 전환. IDT를 참조하여 `intr30_stub` 실행.

**커널 공간 (Kernel Space)**

5.  **저수준 핸들러 (`threads/intr-stubs.S`)**:
-   `intr30_stub`이 인터럽트 번호를 스택에 `push`.
-   `intr_entry`가 모든 사용자 레지스터를 **커널 스택**에 저장 (`struct intr_frame` 형성).
6.  **고수준 핸들러 (`threads/interrupt.c`)**:
-   `intr_handler`가 호출됨.
-   `intr_frame`의 인터럽트 번호(0x30)를 확인.
-   `intr_handlers[0x30]`에 등록된 함수 포인터를 찾아 호출.
7.  **시스템 콜 핸들러 (`userprog/syscall.c`)**:
-   **`syscall_handler(struct intr_frame *f)`가 마침내 실행됨.**

`syscall_handler`는 인자로 받은 `f` (즉, `intr_frame`)의 `esp` 멤버를 통해 사용자 스택에 접근하여 시스템 콜 번호와 인자들을 읽어 들이고, `eax` 멤버에 반환 값을 써서 시스템 콜 처리를 완료하게 됩니다. 이 모든 과정이 Pintos 매뉴얼에 기술된 시스템 콜의 원칙을 정확히 따르고 있습니다.

## 3. Analysis of file system

### filesys/file.c 

#### 1. 핵심 자료구조

이 파일의 모든 것은 `struct file` 구조체를 중심으로 동작합니다.

```c
/* An open file. */
struct file 
  {
    struct inode *inode;        /* File's inode. */
    off_t pos;                  /* Current position. */
    bool deny_write;            /* Has file_deny_write() been called? */
  };
```

  - `struct inode *inode`: 이 파일 객체가 가리키는 실제 파일의 메타데이터 및 데이터 블록 정보(inode)에 대한 포인터입니다. 파일의 크기, 디스크 상의 위치 등의 정보는 모두 이 inode를 통해 접근합니다.
  - `off_t pos`: \*\*파일 내의 현재 위치(offset)\*\*를 나타냅니다. `file_read`나 `file_write`가 호출되면 이 `pos` 위치부터 데이터를 읽거나 씁니다. 같은 파일을 여러 프로세스가 열더라도, 각 `struct file` 객체는 자신만의 `pos` 값을 가지므로 서로 다른 위치를 읽고 쓸 수 있습니다.
  - `bool deny_write`: 이 파일이 쓰기 금지 상태인지 나타내는 플래그입니다. Project 2에서 실행 중인 실행 파일에 대한 쓰기를 방지하는 데 사용됩니다.

#### 2. 주요 함수 분석

**파일 생명주기 관리 함수**

  - `struct file *file_open (struct inode *inode)`

      - 주어진 `inode`를 가지고 새로운 `struct file` 객체를 메모리에 할당하여 생성합니다.
      - `pos`를 0으로 초기화하고, `deny_write`를 `false`로 설정합니다.
      - 파일을 열 때 가장 기본이 되는 함수입니다. `open` 시스템 콜은 궁극적으로 이 함수를 호출하게 됩니다.

  - `struct file *file_reopen (struct file *file)`

      - 이미 열려있는 `file` 객체와 동일한 inode를 가리키는 새로운 `struct file` 객체를 생성합니다.
      - 내부적으로 `inode_reopen()`을 호출하여 inode의 오픈 카운트를 증가시키고, `file_open`을 통해 새로운 `file` 구조체를 만듭니다.
      - 이렇게 생성된 두 `file` 객체는 같은 파일을 가리키지만, `pos`와 같은 상태는 독립적으로 가집니다.

  - `void file_close (struct file *file)`

      - 열려있는 `file` 객체를 닫습니다.
      - `inode_close()`를 호출하여 inode의 오픈 카운트를 감소시키고, `struct file` 객체가 차지하던 메모리를 해제합니다.

**데이터 입출력 함수**

  - `off_t file_read (struct file *file, void *buffer, off_t size)`

      - \*\*현재 위치(`file->pos`)\*\*부터 `size` 바이트만큼 데이터를 읽어 `buffer`에 저장합니다.
      - 실제 데이터 읽기 작업은 `inode_read_at()` 함수에 위임합니다.
      - 읽은 바이트 수만큼 `file->pos`를 **증가시킵니다.**
      - `read` 시스템 콜이 사용하는 핵심 함수입니다.

  - `off_t file_write (struct file *file, const void *buffer, off_t size)`

      - \*\*현재 위치(`file->pos`)\*\*부터 `buffer`에 있는 데이터를 `size` 바이트만큼 파일에 씁니다.
      - 실제 데이터 쓰기 작업은 `inode_write_at()` 함수에 위임합니다.
      - 쓴 바이트 수만큼 `file->pos`를 **증가시킵니다.**
      - `write` 시스템 콜이 사용하는 핵심 함수입니다.

  - `off_t file_read_at (...)` 및 `off_t file_write_at (...)`

      - `file_read`/`write`와 달리, 파일의 현재 위치(`file->pos`)를 사용하지 않고 명시적으로 주어진 `file_ofs` 위치에서 읽거나 씁니다.
      - 이 함수들은 `file->pos` 값을 **변경하지 않습니다.**

**파일 상태 제어 함수**

  - `void file_seek (struct file *file, off_t new_pos)`

      - 파일의 현재 위치 `file->pos`를 `new_pos`로 변경합니다.
      - `seek` 시스템 콜이 이 함수를 사용합니다.

  - `off_t file_tell (struct file *file)`

      - 파일의 현재 위치 `file->pos`를 반환합니다.
      - `tell` 시스템 콜이 이 함수를 사용합니다.

  - `off_t file_length (struct file *file)`

      - 파일의 전체 크기를 바이트 단위로 반환합니다. 실제 크기 정보는 inode에 있으므로 `inode_length()`를 호출하여 결과를 얻습니다.
      - `filesize` 시스템 콜이 이 함수를 사용합니다.

  - `void file_deny_write (struct file *file)` 및 `void file_allow_write (struct file *file)`

      - 파일에 대한 쓰기를 금지하거나 허용합니다. 내부적으로 `inode_deny_write()`와 `inode_allow_write()`를 호출하여 inode 레벨에서 쓰기 권한을 제어합니다.

#### 3. 시스템 콜과의 연관성

file.c에 정의된 함수들은 시스템 콜 구현에 직접적으로 사용된다.

- open() 시스템 콜 → filesys_open() → file_open()

- close() 시스템 콜 → file_close()

- read() 시스템 콜 → file_read()

- write() 시스템 콜 → file_write()

- filesize() 시스템 콜 → file_length()

- seek() 시스템 콜 → file_seek()

- tell() 시스템 콜 → file_tell()

### filesys/filesys.c

#### 1. 초기화 및 종료 함수

  - `void filesys_init (bool format)`

      - 커널 부팅 시 호출되어 파일 시스템 전체를 초기화합니다.
      - `block_get_role(BLOCK_FILESYS)`를 통해 파일 시스템으로 사용할 블록 디바이스(디스크 파티션)를 가져옵니다.
      - 하위 모듈인 `inode_init()`과 `free_map_init()`을 호출하여 각각을 초기화합니다.
      - `format` 인자가 `true`이면, `do_format()`을 호출하여 디스크를 완전히 포맷하고 새로운 파일 시스템을 만듭니다.
      - `free_map_open()`을 호출하여 디스크의 free map 정보를 메모리로 읽어 들입니다.

  - `void filesys_done (void)`

      - Pintos 시스템이 종료될 때 호출됩니다.
      - `free_map_close()`를 호출하여 변경된 free map 정보(어떤 디스크 블록이 사용 중이고 비어있는지에 대한 정보)를 디스크에 안전하게 저장합니다.

-----

#### 2. 파일 관련 핵심 API 함수

이 함수들은 시스템 콜 핸들러에서 직접 사용될 중요한 인터페이스이다. 모두 **파일 이름을 기반으로 동작**하는 것이 특징입니다.

  - `bool filesys_create (const char *name, off_t initial_size)`

      - 이름이 `name`이고 초기 크기가 `initial_size`인 파일을 생성합니다.
      - 이 함수는 여러 하위 모듈을 \*\*조율(Orchestration)\*\*하는 복합적인 작업을 수행합니다.
        1.  **`dir_open_root()`**: 루트 디렉토리를 엽니다. (Pintos Project 2에서는 모든 파일이 루트 디렉토리에 생성됩니다.)
        2.  **`free_map_allocate()`**: 파일의 메타데이터를 저장할 inode를 위해 디스크에서 비어있는 섹터 하나를 할당받습니다.
        3.  **`inode_create()`**: 할당받은 섹터에 `initial_size` 크기를 갖는 inode를 생성합니다.
        4.  **`dir_add()`**: 루트 디렉토리에 파일 이름(`name`)과 방금 생성한 inode의 디스크 섹터 위치를 한 쌍으로 하는 엔트리(항목)를 추가합니다.
      - 이 과정 중 하나라도 실패하면, `free_map_release()` 등을 통해 지금까지의 작업을 되돌려(cleanup) 일관성을 유지합니다.

  - `struct file *filesys_open (const char *name)`

      - 이름이 `name`인 파일을 엽니다.
      - **`file.c`와 `dir.c`를 연결하는 중요한 함수**입니다.
        1.  **`dir_open_root()`**: 루트 디렉토리를 엽니다.
        2.  **`dir_lookup()`**: 디렉토리에서 `name`에 해당하는 파일 엔트리를 찾아, 그 파일의 inode 정보를 가져옵니다.
        3.  **`file_open()`**: `dir_lookup`을 통해 얻은 inode 포인터를 `file.c`의 `file_open()` 함수에 전달합니다.
        4.  `file_open()`은 이 inode를 바탕으로 `struct file` 객체를 생성하여 반환합니다. 이 객체가 바로 '열린 파일'을 나타냅니다.

  - `bool filesys_remove (const char *name)`

      - 이름이 `name`인 파일을 삭제합니다.
      - `dir_open_root()`로 루트 디렉토리를 연 후, `dir_remove()`를 호출하여 디렉토리에서 해당 파일 엔트리를 제거하는 작업을 위임합니다. `dir_remove`는 내부적으로 inode와 데이터 블록을 해제하는 과정을 처리합니다.

-----

#### 3. 시스템 콜과의 연관성

이 파일의 함수들은 파일 관련 시스템 콜을 구현할 때 직접적으로 호출됩니다.

  - `create()` 시스템 콜 → **`filesys_create()`**
  - `open()` 시스템 콜 → **`filesys_open()`**
  - `remove()` 시스템 콜 → **`filesys_remove()`**

-----

#### 4. 구조적 역할

`filesys.c`는 파일 시스템의 계층 구조에서 상위 계층에 위치합니다.

```
+------------------------------------+
|       시스템 콜 핸들러 (syscall.c)     |  <-- 파일 이름(name)으로 요청
+------------------------------------+
                   ↓
+------------------------------------+
|    파일 시스템 API (filesys.c)       |  <-- file, dir, inode, free-map 모듈을 조율
+------------------------------------+
                   ↓
+------------------------------------+
| file.c, inode.c, dir.c, free-map.c |  <-- 실제 작업을 수행하는 하위 모듈들
+------------------------------------+
```

### filesys/inode.c

네, 파일 시스템의 가장 낮은 계층 중 하나인 `filesys/inode.c` 파일을 분석하겠습니다. 이 파일은 파일의 \*\*메타데이터(metadata)\*\*와 디스크 상의 **데이터 블록 위치**를 관리하는 핵심적인 역할을 합니다. `file.c`가 '열린 파일의 상태'를 관리하고 `filesys.c`가 '파일 이름'을 관리했다면, `inode.c`는 **'파일의 실체'** 그 자체를 디스크 수준에서 다룹니다.

### `pintos/src/filesys/inode.c` 코드 분석

#### 1. 핵심 자료구조: 두 종류의 Inode

`struct inode_disk`: 디스크 상의 아이노드**

```c
/* On-disk inode. Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;       /* First data sector. */
    off_t length;               /* File size in bytes. */
    unsigned magic;             /* Magic number. */
    uint32_t unused[125];       /* Not used. */
  };
```

  - **역할**: 파일의 메타데이터를 담아 **디스크에 직접 저장**되는 구조체입니다. 크기는 항상 디스크 섹터 크기(512바이트)와 정확히 일치해야 합니다.
  - `start`: 이 파일의 데이터가 시작되는 **첫 번째 데이터 섹터의 번호**를 가리킵니다. (Project 2의 기본 파일 시스템은 파일 데이터가 디스크 상에 연속적으로 저장된다고 가정합니다.)
  - `length`: 파일의 실제 크기 (바이트 단위).
  - `magic`: `INODE_MAGIC` (0x494e4f44) 값을 가져, 이 섹터가 유효한 inode임을 확인하는 데 사용됩니다.

`struct inode`: 메모리 상의 아이노드**

```c
/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;      /* Element in inode list. */
    block_sector_t sector;      /* Sector number of disk location. */
    int open_cnt;               /* Number of openers. */
    bool removed;               /* True if deleted, false otherwise. */
    int deny_write_cnt;         /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;     /* Inode content. */
  };
```

  - **역할**: 디스크에 있는 inode가 `inode_open()`을 통해 열렸을 때, **메모리에 생성되는** 구조체입니다. `inode_disk`의 복사본(`data` 멤버)과 함께, 실행 중에 필요한 여러 관리 정보를 포함합니다.
  - `sector`: 이 inode 자체가 디스크의 몇 번 섹터에 저장되어 있는지를 나타냅니다.
  - `open_cnt`: **참조 카운트(Reference Count)**. 이 inode가 현재 몇 번이나 열렸는지를 추적합니다. `inode_open()` 시 1 증가하고 `inode_close()` 시 1 감소합니다.
  - `removed`: 파일이 삭제되었는지 여부를 나타내는 플래그입니다. `true`일 경우, `open_cnt`가 0이 되는 순간 이 inode와 관련된 모든 데이터 블록이 디스크에서 해제됩니다.
  - `deny_write_cnt`: 쓰기 금지 요청이 몇 번 있었는지를 카운트합니다. 이 값이 0보다 크면 쓰기가 금지됩니다.

-----

#### 2. 주요 함수 분석

**Inode 생명주기 관리**

  - `bool inode_create (block_sector_t sector, off_t length)`

      - `filesys_create`에 의해 호출됩니다.
      - `free_map_allocate()`를 사용해 `length` 만큼의 데이터 블록을 디스크에서 할당받습니다.
      - `inode_disk` 구조체를 생성하고, 할당받은 데이터 블록의 시작 주소(`start`)와 파일 크기(`length`)를 채웁니다.
      - 이 `inode_disk` 구조체를 `sector` 번호의 디스크 위치에 씁니다.

  - `struct inode *inode_open (block_sector_t sector)`

      - `sector` 번호에 위치한 `inode_disk`를 디스크에서 읽어와, 메모리 상의 `struct inode` 객체를 생성하여 반환합니다.
      - **최적화**: `open_inodes`라는 전역 리스트를 확인하여, 만약 요청된 `sector`의 inode가 이미 메모리에 열려 있다면 디스크를 다시 읽지 않고 기존 `struct inode`의 `open_cnt`만 증가시켜 반환합니다.

  - `void inode_close (struct inode *inode)`

      - `open_cnt`를 1 감소시킵니다.
      - 만약 `open_cnt`가 **0**이 되면, 이 inode를 더 이상 아무도 사용하지 않는다는 의미이므로 다음을 수행합니다.
        1.  메모리에서 `struct inode` 객체를 제거하고 해제합니다.
        2.  만약 `inode->removed` 플래그가 `true`였다면, `free_map_release()`를 호출하여 이 inode가 사용하던 **모든 디스크 블록(inode 자체와 데이터 블록들)을 해제**합니다. 이것이 "열려있는 파일 삭제"의 핵심 로직입니다.

  - `void inode_remove (struct inode *inode)`

      - 실제로 디스크에서 파일을 즉시 삭제하지 않습니다.
      - 단지 `inode->removed` 플래그를 `true`로 설정하기만 합니다. 실제 삭제는 마지막으로 이 파일을 열고 있던 주체가 `inode_close()`를 호출하는 시점으로 지연됩니다.

**데이터 입출력 (I/O)**

  - `off_t inode_read_at (struct inode *inode, void *buffer, off_t size, off_t offset)`

      - `file_read`에 의해 호출됩니다.
      - 파일의 `offset` 위치에서 `size` 바이트만큼 데이터를 읽어 `buffer`에 저장하는 **저수준 I/O의 핵심**입니다.
      - `byte_to_sector()`를 통해 `offset`이 디스크의 몇 번 섹터에 해당하는지 계산합니다.
      - 요청된 `size`와 `offset`이 섹터 경계와 일치하지 않는 경우(예: 섹터 중간부터 읽기 시작), **"bounce buffer"** 라는 임시 메모리 공간을 사용합니다.
        1.  디스크에서 섹터 전체를 bounce buffer로 읽어옵니다.
        2.  Bounce buffer에서 필요한 부분만 `buffer`로 복사합니다.
      - 이 과정을 필요한 모든 섹터에 대해 반복합니다.

  - `off_t inode_write_at (...)`

      - `file_write`에 의해 호출됩니다.
      - `inode_read_at`과 유사한 방식으로, 주어진 `offset`에 데이터를 씁니다. Bounce buffer를 사용하는 이유도 동일합니다. (섹터의 일부만 수정할 때, 나머지 부분을 보존하기 위해 섹터 전체를 읽어서 수정한 뒤 다시 써야 하기 때문입니다.)

-----

#### 3. 구조적 역할

`inode.c`는 파일 시스템의 가장 낮은 논리적 계층으로, 상위 계층(`file.c`, `filesys.c`)과 물리적인 디스크 블록(`block.c`, `free-map.c`) 사이의 다리 역할을 합니다.

```
+------------------------------------+
|        filesys.c / file.c          |  <-- 파일 이름, 파일 오프셋(pos) 등 고수준 개념
+------------------------------------+
                   ↓
+------------------------------------+
|             inode.c                |  <-- 파일 메타데이터, 데이터 블록 위치 등 디스크 구조 관리
+------------------------------------+
                   ↓
+------------------------------------+
|      block.c / free-map.c          |  <-- 물리적인 디스크 섹터 읽기/쓰기, 할당/해제
+------------------------------------+
```

## 4. Proposed Design

### Process Termination Messages