/* SPDX-License-Identifier: MIT */
/*
 * Function names that dladdr() can't give us.
 *
 * dladdr() only searches exported (dynamic) symbols. Static functions,
 * and every function in a program linked without -rdynamic, are only in
 * the ELF .symtab section of the file on disk, so read that directly.
 * Only open/fstat/mmap are used, which keeps it usable in the crash
 * handler too.
 */
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "heapfdr.h"

#define MAX_IMAGES 16

#if __ELF_NATIVE_CLASS == 64
#define NATIVE_CLASS ELFCLASS64
#define SYM_TYPE     ELF64_ST_TYPE
#else
#define NATIVE_CLASS ELFCLASS32
#define SYM_TYPE     ELF32_ST_TYPE
#endif

struct image {
    char                  path[256];
    const unsigned char  *map;
    size_t                size;
    const ElfW(Sym)      *syms;
    size_t                nsyms;
    const char           *strtab;
    size_t                strsize;
};

static struct image images[MAX_IMAGES];
static int nimages;
static char busy;   /* plain flag: if someone is already in here, give up instead of waiting */

static int in_bounds(const struct image *im, uint64_t off, uint64_t len)
{
    return off <= im->size && len <= im->size - off;
}

/* Find .symtab and its string table. Leaves syms NULL if there is none. */
static void parse(struct image *im)
{
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)im->map;
    const ElfW(Shdr) *sh;

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != NATIVE_CLASS)
        return;
    if (eh->e_shentsize != sizeof(ElfW(Shdr)) ||
        !in_bounds(im, eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(ElfW(Shdr))))
        return;

    sh = (const ElfW(Shdr) *)(im->map + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        const ElfW(Shdr) *str;

        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum)
            continue;
        str = &sh[sh[i].sh_link];
        if (!in_bounds(im, sh[i].sh_offset, sh[i].sh_size) ||
            !in_bounds(im, str->sh_offset, str->sh_size))
            return;
        im->syms = (const ElfW(Sym) *)(im->map + sh[i].sh_offset);
        im->nsyms = sh[i].sh_size / sizeof(ElfW(Sym));
        im->strtab = (const char *)(im->map + str->sh_offset);
        im->strsize = str->sh_size;
        return;
    }
}

static struct image *open_image(const char *path)
{
    struct image *im;
    struct stat st;
    int fd;
    void *m;

    for (int i = 0; i < nimages; i++)
        if (strcmp(images[i].path, path) == 0)
            return &images[i];
    if (nimages == MAX_IMAGES || strlen(path) >= sizeof images[0].path)
        return NULL;

    im = &images[nimages++];
    memset(im, 0, sizeof *im);
    strcpy(im->path, path);

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return im;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(ElfW(Ehdr))) {
        m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            im->map = m;
            im->size = (size_t)st.st_size;
            parse(im);
            if (!im->syms) {
                munmap(m, im->size);
                im->map = NULL;
            }
        }
    }
    close(fd);
    return im;
}

const char *elf_symbol(const char *path, uintptr_t file_addr, uintptr_t *offset)
{
    const char *name = NULL;
    struct image *im;

    if (__atomic_test_and_set(&busy, __ATOMIC_ACQUIRE))
        return NULL;

    im = open_image(path);
    if (im && im->syms) {
        for (size_t i = 0; i < im->nsyms; i++) {
            const ElfW(Sym) *s = &im->syms[i];

            if (SYM_TYPE(s->st_info) != STT_FUNC || s->st_shndx == SHN_UNDEF ||
                s->st_size == 0 || s->st_name >= im->strsize)
                continue;
            if (file_addr >= s->st_value && file_addr < s->st_value + s->st_size) {
                name = im->strtab + s->st_name;
                *offset = file_addr - s->st_value;
                break;
            }
        }
    }

    __atomic_clear(&busy, __ATOMIC_RELEASE);
    return name;
}

void elfsym_child_after_fork(void)
{
    __atomic_clear(&busy, __ATOMIC_RELEASE);
}
