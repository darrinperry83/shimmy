set pagination off
set confirm off
catch syscall
commands
silent
printf "LINUX_SHELL_SYSCALL=%ld\n", $x8
continue
end
run --noprofile --norc -c 'printf "ARCH_SHELL_REFERENCE_PASS\n"; pwd; test 2 -eq 2'
