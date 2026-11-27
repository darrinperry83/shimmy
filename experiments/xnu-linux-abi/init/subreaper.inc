/* Native child-list adoption, included in kern_exit.c. Caller holds list lock.
 * Until explicit init domains exist, stop at the first non-Linux ancestor.
 * No synthetic child/zombie table: XNU owns parentage, SIGCHLD and reaping. */
static proc_t
rd_linux_subreaper_find_locked(proc_t exiting)
{
    if (proc_is_shadow(exiting) ||
        !(os_atomic_load(&exiting->p_ladvflag, relaxed) & P_RDXNU_LINUX))
        return PROC_NULL;
    for (proc_t ancestor = exiting->p_pptr;
        ancestor != PROC_NULL && ancestor != initproc; ancestor = ancestor->p_pptr) {
        if (!(os_atomic_load(&ancestor->p_ladvflag, relaxed) & P_RDXNU_LINUX))
            break;
        if (rd_linux_subreaper_get(ancestor) && !(ancestor->p_lflag & P_LEXIT)) {
            proc_t held = proc_ref(ancestor, true);
            if (held != PROC_NULL) return held;
        }
    }
    return PROC_NULL;
}

static bool
rd_linux_subreaper_adopt_locked(proc_t adopter, proc_t child)
{
    if (adopter == PROC_NULL || (child->p_lflag & P_LTRACED) ||
        !(os_atomic_load(&child->p_ladvflag, relaxed) & P_RDXNU_LINUX))
        return false;
    if (child->p_stat == SZOMB) {
        /* Follow the native exit loop's exclusion against concurrent waits. */
        while (child->p_listflag & P_LIST_WAITING)
            msleep(&child->p_stat, &proc_list_mlock, PWAIT, "linux_adopt", 0);
        pid_t pid = proc_getpid(child);
        uid_t uid = kauth_cred_getruid(proc_ucred_unsafe(child));
        int status = child->p_xstat;
        child->p_listflag &= ~P_LIST_DEADPARENT;
        proc_reparentlocked(child, adopter, 0, 1);
        /* A different adopter thread can reap child as soon as we unlock.
         * Only copied event values may be used after this point. */
        proc_list_unlock();
        proc_lock(adopter);
        adopter->si_pid = pid;
        adopter->si_uid = uid;
        adopter->si_status = status;
        adopter->si_code = CLD_EXITED;
        proc_unlock(adopter);
        psignal(adopter, SIGCHLD);
        wakeup(adopter);
        proc_list_lock();
    } else {
        child->p_listflag &= ~P_LIST_DEADPARENT;
        proc_reparentlocked(child, adopter, 0, 1);
    }
    return true;
}
