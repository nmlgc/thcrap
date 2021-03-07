/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Random utility functions.
  */

#include "thcrap.h"
#include <errno.h>

size_t dword_align(const size_t val)
{
	return (val + 3) & ~3;
}

BYTE* ptr_dword_align(const BYTE *in)
{
	return (BYTE*)dword_align((UINT_PTR)in);
}

size_t ptr_advance(const unsigned char **src, size_t num)
{
	*src += num;
	return num;
}

size_t memcpy_advance_src(unsigned char *dst, const unsigned char **src, size_t num)
{
	memcpy(dst, *src, num);
	return ptr_advance(src, num);
}

void str_ascii_replace(char *str, const char from, const char to)
{
	if(str) {
		char *p = str;
		while(*p) {
			if(p[0] == from) {
				p[0] = to;
			}
			p++;
		}
	}
}

void str_slash_normalize(char *str)
{
	str_ascii_replace(str, '\\', '/');
}

void str_slash_normalize_win(char *str)
{
	str_ascii_replace(str, '/', '\\');
}

unsigned int str_num_digits(int number)
{
	unsigned int digits = 0;
	if(number < 0) {
		digits = 1; // remove this line if '-' counts as a digit
	}
	while(number) {
		number /= 10;
		digits++;
	}
	return digits;
}

int str_num_base(const char *str)
{
	return !strnicmp(str, "0x", 2) ? 16 : 10;
}

void str_hexdate_format(char format[11], uint32_t date)
{
	sprintf(format, "%04x-%02x-%02x",
		(date & 0xffff0000) >> 16,
		(date & 0x0000ff00) >> 8,
		(date & 0x000000ff)
	);
}

extern "C" char* strndup(const char* src, size_t size) {
	char* ret = (char*)malloc(size + 1);
	if (!ret) return NULL;
	// strncpy will 0 pad
	if (!_memccpy(ret, src, '\0', size)) {
		ret[size] = '\0';
	}
	return ret;
}

size_t str_address_value(const char *str, HMODULE hMod, str_address_ret_t *ret)
{
	int base = 0;
	size_t val = 0;
	char *endptr_temp;
	char **endptr = ret ? (char **)&ret->endptr : &endptr_temp;

	if(str[0] != '\0') {
		// Module-relative hex values
		if(!strnicmp(str, "Rx", 2)) {
			val = (size_t)(hMod ? hMod : GetModuleHandle(NULL));
			base = 16;
			str += 2;
		}
		else if (!strnicmp(str, "0x", 2)) {
			base = 16;
			str += 2;
		}
	}
	errno = 0;
	if (base != 0) {
		val += strtoul(str, endptr, base);
	} else {
		const size_t temp_val1 = strtoul(str, &endptr_temp, 10);
		char* endptr_temp2;
		const size_t temp_val2 = strtoul(str, &endptr_temp2, 16);
		val += endptr_temp == endptr_temp2 ? temp_val1 : temp_val2;
		*endptr = endptr_temp == endptr_temp2 ? endptr_temp : endptr_temp2;
	}

	if(ret) {
		ret->error = STR_ADDRESS_ERROR_NONE;

		if(errno == ERANGE) {
			ret->error |= STR_ADDRESS_ERROR_OVERFLOW;
		}
		if(*endptr[0] != '\0') {
			ret->error |= STR_ADDRESS_ERROR_GARBAGE;
		}
	}
	return val;
}
