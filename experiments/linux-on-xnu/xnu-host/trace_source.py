"""Opt-in source checkpoints for the pinned Linux memory bootstrap only."""


def instrument(source):
    changes = [
        ('\tmem_size = mem_sz;', '\tlkl_printf("CORE bootmem size=%lu\\n", mem_sz);\n\tmem_size = mem_sz;'),
        ('\tmemory_start = (unsigned long)_memory_start;', '\tlkl_printf("CORE RAM allocated=%p\\n", _memory_start);\n\tmemory_start = (unsigned long)_memory_start;'),
        ('\tmemblock_add(__pa(memory_start), mem_size);', '\tlkl_printf("CORE before memblock_add\\n");\n\tmemblock_add(__pa(memory_start), mem_size);\n\tlkl_printf("CORE after memblock_add\\n");'),
        ('\tempty_zero_page = memblock_alloc(PAGE_SIZE, PAGE_SIZE);', '\tlkl_printf("CORE before zero page allocation\\n");\n\tempty_zero_page = memblock_alloc(PAGE_SIZE, PAGE_SIZE);\n\tlkl_printf("CORE zero page=%p\\n", empty_zero_page);'),
        ('\tfree_area_init(zones_max_pfn);', '\tlkl_printf("CORE before free_area_init\\n");\n\tfree_area_init(zones_max_pfn);\n\tlkl_printf("CORE after free_area_init\\n");'),
    ]
    for before, after in changes:
        if source.count(before) != 1:
            raise ValueError(f'Linux memory-bootstrap trace anchor changed: {before!r}')
        source = source.replace(before, after)
    return source
