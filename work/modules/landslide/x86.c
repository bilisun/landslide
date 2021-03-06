/**
 * @file x86.c
 * @brief x86-specific utilities
 * @author Ben Blum
 */

#include <simics/api.h>

#define MODULE_NAME "X86"
#define MODULE_COLOUR COLOUR_DARK COLOUR_GREEN

#include "common.h"
#include "kernel_specifics.h"
#include "kspec.h"
#include "landslide.h"
#include "student_specifics.h"
#include "x86.h"

/* Horribly, simics's attributes for the segsels are lists instead of ints. */
#define GET_SEGSEL(cpu, name) \
	SIM_attr_integer(SIM_attr_list_item(SIM_get_attribute(cpu, #name), 0))

/* two possible methods for causing a timer interrupt - the "immediately"
 * version makes the simulation immediately jump to some assembly on the stack
 * that directly invokes the timer interrupt INSTEAD of executing the pending
 * instruction; the other way just manipulates the cpu's interrupt pending
 * flags to make it do the interrupt itself. */
unsigned int cause_timer_interrupt_immediately(conf_object_t *cpu)
{
	unsigned int esp = GET_CPU_ATTR(cpu, esp);
	unsigned int eip = GET_CPU_ATTR(cpu, eip);
	unsigned int eflags = GET_CPU_ATTR(cpu, eflags);
	unsigned int handler = kern_get_timer_wrap_begin();

	if (KERNEL_MEMORY(eip)) {
		/* Easy mode. Just make a small iret stack frame. */
		assert(GET_SEGSEL(cpu, cs) == SEGSEL_KERNEL_CS);
		assert(GET_SEGSEL(cpu, ss) == SEGSEL_KERNEL_DS);

		lsprintf(DEV, "tock! (0x%x)\n", eip);

		/* 12 is the size of an IRET frame only when already in kernel mode. */
		unsigned int new_esp = esp - 12;
		SET_CPU_ATTR(cpu, esp, new_esp);
		write_memory(cpu, new_esp + 8, eflags, 4);
		write_memory(cpu, new_esp + 4, SEGSEL_KERNEL_CS, 4);
		write_memory(cpu, new_esp + 0, eip, 4);
	} else {
		/* Hard mode - do a mode switch also. Grab esp0, make a large
		 * iret frame, and change the segsel registers to kernel mode. */
		assert(GET_SEGSEL(cpu, cs) == SEGSEL_USER_CS);
		assert(GET_SEGSEL(cpu, ss) == SEGSEL_USER_DS);

		lsprintf(DEV, "tock! from userspace! (0x%x)\n", eip);

		unsigned int esp0 =
#ifdef PINTOS_KERNEL
			0;
		// FIXME
		assert(false && "TSS/esp0 not implemented for Pintos.");
#else
			READ_MEMORY(cpu, (unsigned)GUEST_ESP0_ADDR);
#endif
		/* 20 is the size of an IRET frame coming from userland. */
		unsigned int new_esp = esp0 - 20;
		SET_CPU_ATTR(cpu, esp, new_esp);
		write_memory(cpu, new_esp + 16, SEGSEL_USER_DS, 4);
		write_memory(cpu, new_esp + 12, esp, 4);
		write_memory(cpu, new_esp +  8, eflags, 4);
		write_memory(cpu, new_esp +  4, SEGSEL_USER_CS, 4);
		write_memory(cpu, new_esp +  0, eip, 4);

		/* Change %cs and %ss. (Other segsels should be saved/restored
		 * in the kernel's handler wrappers.) */
		attr_value_t cs = SIM_make_attr_list(10,
			SIM_make_attr_integer(SEGSEL_KERNEL_CS),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(0),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(11),
			SIM_make_attr_integer(0),
			SIM_make_attr_integer(4294967295L),
			SIM_make_attr_integer(1));
		set_error_t ret = SIM_set_attribute(cpu, "cs", &cs);
		assert(ret == Sim_Set_Ok && "failed set cs");
		SIM_free_attribute(cs);
		attr_value_t ss = SIM_make_attr_list(10,
			SIM_make_attr_integer(SEGSEL_KERNEL_DS),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(3),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(1),
			SIM_make_attr_integer(3),
			SIM_make_attr_integer(0),
			SIM_make_attr_integer(4294967295L),
			SIM_make_attr_integer(1));
		ret = SIM_set_attribute(cpu, "ss", &ss);
		assert(ret == Sim_Set_Ok && "failed set ss");
		SIM_free_attribute(ss);

		/* Change CPL. */
		assert(GET_CPU_ATTR(cpu, cpl) == 3);
		SET_CPU_ATTR(cpu, cpl, 0);

	}
	SET_CPU_ATTR(cpu, eip, handler);
#ifdef PINTOS_KERNEL
	SET_CPU_ATTR(cpu, eflags, GET_CPU_ATTR(cpu, eflags) & ~EFL_IF);
#endif
	return handler;
}

/* i.e., with stallin' */
static void cause_timer_interrupt_soviet_style(conf_object_t *cpu, lang_void *x)
{
	SIM_stall_cycle(cpu, 0);
}

void cause_timer_interrupt(conf_object_t *cpu)
{
	lsprintf(DEV, "tick! (0x%x)\n", GET_CPU_ATTR(cpu, eip));

	if (GET_CPU_ATTR(cpu, pending_vector_valid)) {
		SET_CPU_ATTR(cpu, pending_vector,
			     GET_CPU_ATTR(cpu, pending_vector)
			     | TIMER_INTERRUPT_NUMBER);
	} else {
		SET_CPU_ATTR(cpu, pending_vector, TIMER_INTERRUPT_NUMBER);
		SET_CPU_ATTR(cpu, pending_vector_valid, 1);
	}

	SET_CPU_ATTR(cpu, pending_interrupt, 1);
	/* Causes simics to flush whatever pipeline, implicit or not, would
	 * otherwise let more instructions get executed before the interrupt be
	 * taken. */
	SIM_run_unrestricted(cpu, cause_timer_interrupt_soviet_style, NULL);
}

/* Will use 8 bytes of stack when it runs. */
#define CUSTOM_ASSEMBLY_CODES_STACK 8
static const char custom_assembly_codes[] = {
	0x50, /* push %eax */
	0x52, /* push %edx */
	0x66, 0xba, 0x20, 0x00, /* mov $0x20, %dx # INT_ACK_CURRENT */
	0xb0, 0x20, /* mov $0x20, %al # INT_CTL_PORT */
	0xee, /* out %al, (%dx) */
	0x5a, /* pop %edx */
	0x58, /* pop %eax */
	0xcf, /* iret */
};

unsigned int avoid_timer_interrupt_immediately(conf_object_t *cpu)
{
	// XXX: This mechanism is vulnerable to the twilight zone bug that's
	// fixed in delay_instruction; as well as the stack-clobber bug from
	// issue #201. It's just 10000x less likely to trigger because of how
	// infrequently simics timer ticks happen.
	unsigned int buf = GET_CPU_ATTR(cpu, esp) -
		(ARRAY_SIZE(custom_assembly_codes) + CUSTOM_ASSEMBLY_CODES_STACK);

	lsprintf(INFO, "Cuckoo!\n");

	STATIC_ASSERT(ARRAY_SIZE(custom_assembly_codes) % 4 == 0);
	for (int i = 0; i < ARRAY_SIZE(custom_assembly_codes); i++) {
		write_memory(cpu, buf + i, custom_assembly_codes[i], 1);
	}

	SET_CPU_ATTR(cpu, eip, buf);
	return buf;
}

/* keycodes for the keyboard buffer */
static int i8042_key(char c)
{
	static const int i8042_keys[] = {
		['0'] = 18, ['1'] = 19, ['2'] = 20, ['3'] = 21, ['4'] = 22,
		['5'] = 23, ['6'] = 24, ['7'] = 25, ['8'] = 26, ['9'] = 27,
		['a'] = 28, ['b'] = 29, ['c'] = 30, ['d'] = 31, ['e'] = 32,
		['f'] = 33, ['g'] = 34, ['h'] = 35, ['i'] = 36, ['j'] = 37,
		['k'] = 38, ['l'] = 39, ['m'] = 40, ['n'] = 41, ['o'] = 42,
		['p'] = 43, ['q'] = 44, ['r'] = 45, ['s'] = 46, ['t'] = 47,
		['u'] = 48, ['v'] = 49, ['w'] = 50, ['x'] = 51, ['y'] = 52,
		['z'] = 53, ['\''] = 54, [','] = 55, ['.'] = 56, [';'] = 57,
		['='] = 58, ['/'] = 59, ['\\'] = 60, [' '] = 61, ['['] = 62,
		[']'] = 63, ['-'] = 64, ['`'] = 65,             ['\n'] = 67,
	};
	assert(i8042_keys[(int)c] != 0 && "Attempt to type an unsupported key");
	return i8042_keys[(int)c];
}

static bool i8042_shift_key(char *c)
{
	static const char i8042_shift_keys[] = {
		['~'] = '`', ['!'] = '1', ['@'] = '2', ['#'] = '3', ['$'] = '4',
		['%'] = '5', ['^'] = '6', ['&'] = '7', ['*'] = '8', ['('] = '9',
		[')'] = '0', ['_'] = '-', ['+'] = '=', ['Q'] = 'q', ['W'] = 'w',
		['E'] = 'e', ['R'] = 'r', ['T'] = 't', ['Y'] = 'y', ['U'] = 'u',
		['I'] = 'i', ['O'] = 'o', ['P'] = 'p', ['{'] = '[', ['}'] = ']',
		['A'] = 'a', ['S'] = 's', ['D'] = 'd', ['D'] = 'd', ['F'] = 'f',
		['G'] = 'g', ['H'] = 'h', ['J'] = 'j', ['K'] = 'k', ['L'] = 'l',
		[':'] = ';', ['"'] = '\'', ['Z'] = 'z', ['X'] = 'x',
		['C'] = 'c', ['V'] = 'v', ['B'] = 'b', ['N'] = 'n', ['M'] = 'm',
		['<'] = ',', ['>'] = '.', ['?'] = '/',
	};

	if (i8042_shift_keys[(int)*c] != 0) {
		*c = i8042_shift_keys[(int)*c];
		return true;
	}
	return false;
}

void cause_keypress(conf_object_t *kbd, char key)
{
	bool do_shift = i8042_shift_key(&key);

	unsigned int keycode = i8042_key(key);

	attr_value_t i = SIM_make_attr_integer(keycode);
	attr_value_t v = SIM_make_attr_integer(0); /* see i8042 docs */
	/* keycode value for shift found by trial and error :< */
	attr_value_t shift = SIM_make_attr_integer(72);

	set_error_t ret;

	/* press key */
	if (do_shift) {
		ret = SIM_set_attribute_idx(kbd, "key_event", &shift, &v);
		assert(ret == Sim_Set_Ok && "shift press failed!");
	}

	ret = SIM_set_attribute_idx(kbd, "key_event", &i, &v);
	assert(ret == Sim_Set_Ok && "cause_keypress press failed!");

	/* release key */
	v = SIM_make_attr_integer(1);
	ret = SIM_set_attribute_idx(kbd, "key_event", &i, &v);
	assert(ret == Sim_Set_Ok && "cause_keypress release failed!");

	if (do_shift) {
		ret = SIM_set_attribute_idx(kbd, "key_event", &shift, &v);
		assert(ret == Sim_Set_Ok && "cause_keypress release failed!");
	}
}

bool interrupts_enabled(conf_object_t *cpu)
{
	unsigned int eflags = GET_CPU_ATTR(cpu, eflags);
	return (eflags & EFL_IF) != 0;
}

static bool mem_translate(conf_object_t *cpu, unsigned int addr, unsigned int *result)
{
#ifdef PINTOS_KERNEL
	/* In pintos the kernel is mapped at 3 GB, not direct-mapped. Luckily,
	 * paging is enabled in start(), while landslide enters at main(). */
	assert((GET_CPU_ATTR(cpu, cr0) & CR0_PG) != 0 &&
	       "Expected Pintos to have paging enabled before landslide entrypoint.");
#else
	/* In pebbles the kernel is direct-mapped and paging may not be enabled
	 * until after landslide starts recording instructions. */
	if (KERNEL_MEMORY(addr)) {
		/* assume kern mem direct-mapped -- not strictly necessary */
		*result = addr;
		return true;
	} else if ((GET_CPU_ATTR(cpu, cr0) & CR0_PG) == 0) {
		/* paging disabled; cannot translate user address */
		return false;
	}
#endif

	unsigned int upper = addr >> 22;
	unsigned int lower = (addr >> 12) & 1023;
	unsigned int offset = addr & 4095;
	unsigned int cr3 = GET_CPU_ATTR(cpu, cr3);
	unsigned int pde_addr = cr3 + (4 * upper);
	unsigned int pde = SIM_read_phys_memory(cpu, pde_addr, WORD_SIZE);
	assert(SIM_get_pending_exception() == SimExc_No_Exception &&
	       "failed memory read during VM translation -- kernel VM bug?");
	/* check present bit of pde to not anger the simics gods */
	if ((pde & 0x1) == 0) {
		return false;
#ifdef PDE_PTE_POISON
	} else if (pde == PDE_PTE_POISON) {
		return false;
#endif
	}
	unsigned int pte_addr = (pde & ~4095) + (4 * lower);
	unsigned int pte = SIM_read_phys_memory(cpu, pte_addr, WORD_SIZE);
	assert(SIM_get_pending_exception() == SimExc_No_Exception &&
	       "failed memory read during VM translation -- kernel VM bug?");
	/* check present bit of pte to not anger the simics gods */
	if ((pte & 0x1) == 0) {
		return false;
#ifdef PDE_PTE_POISON
	} else if (pte == PDE_PTE_POISON) {
		return false;
#endif
	}
	*result = (pte & ~4095) + offset;
	return true;
}

unsigned int read_memory(conf_object_t *cpu, unsigned int addr, unsigned int width)
{
	unsigned int phys_addr;
	if (mem_translate(cpu, addr, &phys_addr)) {
		unsigned int result = SIM_read_phys_memory(cpu, phys_addr, width);
		assert(SIM_get_pending_exception() == SimExc_No_Exception &&
		       "failed memory read during VM translation -- kernel VM bug?");
		return result;
	} else {
		return 0; /* :( */
	}
}

bool write_memory(conf_object_t *cpu, unsigned int addr, unsigned int val, unsigned int width)
{
	unsigned int phys_addr;
	if (mem_translate(cpu, addr, &phys_addr)) {
		SIM_write_phys_memory(cpu, phys_addr, val, width);
		assert(SIM_get_pending_exception() == SimExc_No_Exception &&
		       "failed memory write during VM translation -- kernel VM bug?");
		return true;
	} else {
		return false;
	}
}

char *read_string(conf_object_t *cpu, unsigned int addr)
{
	unsigned int length = 0;

	while (READ_BYTE(cpu, addr + length) != 0) {
		length++;
	}

	char *buf = MM_XMALLOC(length + 1, char);

	for (unsigned int i = 0; i <= length; i++) {
		buf[i] = READ_BYTE(cpu, addr + i);
	}

	return buf;
}

/* will read at most 3 opcodes */
bool opcodes_are_atomic_swap(uint8_t *ops) {
	unsigned int offset = 0;
	if (ops[offset] == 0xf0) {
		/* lock prefix */
		offset++;
	}

	if (ops[offset] == 0x86 || ops[offset] == 0x87) {
		/* xchg */
		return true;
	} else if (ops[offset] == 0x0f) {
		offset++;
		if (ops[offset] == 0xb0 || ops[offset] == 0xb1) {
			/* cmpxchg */
			return true;
		} else {
			// FIXME: Shouldn't 0F C0 and 0F C1 (xadd) be here?
			return false;
		}
	} else {
		return false;
	}
}

bool instruction_is_atomic_swap(conf_object_t *cpu, unsigned int eip) {
	uint8_t opcodes[3];
	opcodes[0] = READ_BYTE(cpu, eip);
	opcodes[1] = READ_BYTE(cpu, eip + 1);
	opcodes[2] = READ_BYTE(cpu, eip + 2);
	return opcodes_are_atomic_swap(opcodes);
}

/* I figured this out between 3 and 5 AM on a sunday morning. Could have been
 * playing Netrunner instead. Even just writing the PLDI paper would have been
 * more pleasurable. It was a real "twilight zone" bug. Aaaargh. */
static void flush_instruction_cache(lang_void *x)
{
	/* I tried using SIM_flush_I_STC_logical here, and even the supposedly
	 * universal SIM_STC_flush_cache, but the make sucked. h8rs gon h8. */
	SIM_flush_all_caches();
}

/* a similar trick to avoid timer interrupt, but delays by just 1 instruction. */
unsigned int delay_instruction(conf_object_t *cpu)
{
	/* Insert a relative jump, "e9 XXXXXXXX"; 5 bytes. Try to put it just
	 * after _end, but if _end is page-aligned, use some space just below
	 * the stack pointer as a fallback (XXX: this has issue #201). */
	unsigned int buf =
#ifdef PINTOS_KERNEL
		PAGE_SIZE - 1 ; /* dummy value to trigger backup plan */
#else
		GET_SEGSEL(cpu, cs) == SEGSEL_USER_CS ?
			USER_IMG_END : /* use spare .bss in user image */
			PAGE_SIZE - 1 /* FIXME #201 */;
#endif

	bool need_backup_location = buf % PAGE_SIZE > PAGE_SIZE - 8;

	/* Translate buf's virtual location to physical address. */
	unsigned int phys_buf;
	if (!need_backup_location) {
		if (!mem_translate(cpu, buf, &phys_buf)) {
			need_backup_location = true;
		}
	}
	if (need_backup_location) {
		// XXX: See issue #201. This is only safe 99% of the time.
		// To properly fix, need hack the reference kernel.
		buf = GET_CPU_ATTR(cpu, esp);
		assert(buf % PAGE_SIZE >= 8 &&
		       "no spare room under stack; can't delay instruction");
		buf -= 8;
		lsprintf(CHOICE, "WARNING: Need to delay instruction, but no "
			 "spare .bss. Using stack instead -- 0x%x.\n", buf);
		bool mapping_present = mem_translate(cpu, buf, &phys_buf);
		assert(mapping_present && "stack unmapped; can't delay ");
	}

	/* Compute relative offset. Note "e9 00000000" would jump to buf+5. */
	unsigned int offset = GET_CPU_ATTR(cpu, eip) - (buf + 5);

	lsprintf(INFO, "Be back in a jiffy...\n");

	SIM_write_phys_memory(cpu, phys_buf, 0xe9, 1);
	SIM_write_phys_memory(cpu, phys_buf + 1, offset, 4);

	SET_CPU_ATTR(cpu, eip, buf);

	SIM_run_alone(flush_instruction_cache, NULL);

	return buf;
}
