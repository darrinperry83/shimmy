# SEP hibernation base interfaces

The 16 missing SEPHibernation driver imports are now defined by the rebuilt
kernel. Apple’s collection linker accepts that kernel with the complete stock
357-entry set. Runtime SEP operations and hibernation remain unproven.

## Reference and reconstruction

Reference: the verified 25F80 KDK, DEVELOPMENT T6020 kernel and matching RELEASE
T6020 dSYM. Reference kernel SHA-256:
`c91f5d7cbab23d94464ec10b303da4115da31ff45052645788c8a2c7e4be5650`.
The source tree and KDK versions remain pinned in dependencies.json.

The matching dSYM describes the missing classes in Apple's SEPHibernator.h:

- SEPHibernator derives from OSObject, is 0x18 bytes, and has an expansion pointer
  at 0x10. prepareToHibernate takes two opaque SEP payload pointers and is pure
  virtual. The class has eight reserved virtual methods.
- SEPHibernatorService derives from IOService, is 0x90 bytes, and has the public
  hibernator pointer at 0x88.

`kernel/IOKit/SEPHibernator.h` reconstructs those declarations. Constructors,
metaclass registration, allocation and reserved-method behavior use XNU's own
OSMetaClass macros. Reserved methods call the usual reservedCalled failure path;
they do not implement successful operations.

The implementation in kernel/SEPHibernator.cpp follows observed base behavior:

- init delegates to OSObject::init, including the standard allocation tracking.
- The base service start returns true; its machine code is identical to Apple's
  matching base method. This is a base-class callback, not a fabricated SEP or
  hibernation result. Apple's concrete driver supplies its own behavior.
- sepHibernator matches the concrete HibernationService, waits indefinitely,
  checks its SEPHibernatorService class, obtains the pointer at 0x88, and releases
  the matching dictionary and service references. Invalid lookup paths panic.
- prepareToHibernate remains abstract. No firmware command, key generation,
  authentication result or hibernation result is supplied by this reconstruction.

prepare_sep.py installs these sources into the disposable XNU tree, adds the
compilation unit and the required export names, and rejects unrelated source
edits. The export file is validated because comments in that file format can
be interpreted as linker aliases. No global hibernation flag is changed.

## Validation

`audit_sep.py` checks real linked artifacts against the Apple reference:

- All 16 imports used by the real SEPHibernation driver exist.
- Both class and both metaclass vtables match, including slot targets and PAC
  metadata. Trailing alignment padding is excluded from method-slot counts.
- DWARF confirms both object sizes and the member offsets.
- The actual generated getter uses HibernationService, the indefinite wait and
  the 0x88 member access.
- The base start function's machine words equal Apple's implementation.
- The actual SEP operation is still a pure virtual slot.

The general ABI gate now compares 16 vtables / 1,677 method slots, all matching.
Earlier counts included one alignment word per table; the parser now excludes
that padding consistently.

The complete collection link uses every stock boot driver, plus any additional
loaded stock driver. It does not elide SEP, GPU, audio or any other driver.
The output has exactly the stock collection’s 357 entry IDs, with our new kernel
UUID, and passes kmutil inspect. Four raw battery/RAID imports remain visible in
both raw-kernel controls; the successful complete native link is required to
adjudicate them, rather than an ignored-symbol list.

## Limits

This restores base interfaces and proves offline linkage. It does not prove
hardware initialization, SEP communication, suspend/resume, or hibernate/restore.
The public ARM64 build's kernel-wide HIBERNATION configuration remains disabled;
enabling and validating the full hibernation path is separate work. No kernel
collection has been installed or booted, and no firmware has been modified.
