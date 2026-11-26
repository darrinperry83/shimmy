/* VM-only output proof. This is a mirror, not a Linux tty device. */
LCK_MTX_DECLARE(rd_linux_console_lock, &vconsole_lck_grp);
static bool rd_linux_console_started;

void rdxnu_console_begin(void);
void rdxnu_console_output(const char *, size_t);

void
rdxnu_console_begin(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("rdxnu_linux_console", &enabled, sizeof(enabled)) || enabled != 1) return;
    lck_mtx_lock(&rd_linux_console_lock);
    if (!rd_linux_console_started && gc_initialized && vinfo.v_baseaddr &&
        vinfo.v_width && vinfo.v_height && vinfo.v_depth == 32) {
        /* No WindowServer owns the display in this diagnostic boot profile.
         * Use the framebuffer maintained by the platform/graphics driver.
         * Explicit text mode also works when serial=3 selected the UART. */
        initialize_screen(NULL, kPETextMode);
        vc_progress_set(FALSE, 0);
        gc_enable(TRUE);
        gc_acquired = TRUE;
        rd_linux_console_started = true;
        const char *banner = "\033[2J\033[HRDXNU Linux console: output proof\r\n"
            "Linux ELF on XNU / Apple drivers / no WindowServer\r\n"
            "Keyboard and tty input are not implemented yet.\r\n\r\n";
        for (; *banner; ++banner) vcputc(*banner);
        printf("RDXNU_CONSOLE_READY width=%u height=%u depth=%u\n",
            vinfo.v_width, vinfo.v_height, vinfo.v_depth);
    }
    lck_mtx_unlock(&rd_linux_console_lock);
}

void
rdxnu_console_output(const char *bytes, size_t length)
{
    lck_mtx_lock(&rd_linux_console_lock);
    if (rd_linux_console_started) {
        for (size_t i = 0; i < length; ++i) {
            if (bytes[i] == '\n') vcputc('\r');
            vcputc(bytes[i]);
        }
    }
    lck_mtx_unlock(&rd_linux_console_lock);
}
