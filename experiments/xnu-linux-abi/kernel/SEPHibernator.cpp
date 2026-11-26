/* Reconstructed base-class behavior from the matching Apple KDK.
 * No SEP command, key operation, or successful hibernation result is fabricated.
 * See SEP-HIBERNATION.md for the exact reference and remaining validation gates.
 */
#include <IOKit/SEPHibernator.h>
#include <kern/debug.h>

OSDefineMetaClassAndAbstractStructors(SEPHibernator, OSObject)
OSMetaClassDefineReservedUnused(SEPHibernator, 0);
OSMetaClassDefineReservedUnused(SEPHibernator, 1);
OSMetaClassDefineReservedUnused(SEPHibernator, 2);
OSMetaClassDefineReservedUnused(SEPHibernator, 3);
OSMetaClassDefineReservedUnused(SEPHibernator, 4);
OSMetaClassDefineReservedUnused(SEPHibernator, 5);
OSMetaClassDefineReservedUnused(SEPHibernator, 6);
OSMetaClassDefineReservedUnused(SEPHibernator, 7);

OSDefineMetaClassAndStructors(SEPHibernatorService, IOService)

bool SEPHibernator::init()
{
    // Apple's init has the same optional allocation tracking as OSObject::init.
    return OSObject::init();
}

bool SEPHibernatorService::start(IOService *provider)
{
    (void)provider;
    // This is the observed base-class implementation, not a SEP operation.
    // The concrete driver overrides start and prepareToHibernate.
    return true;
}

OSPtr<SEPHibernator> SEPHibernator::sepHibernator()
{
    OSDictionary *matching = IOService::serviceMatching("HibernationService");
    if (!matching) {
        panic("SEPHibernator: unable to create service matching dictionary");
    }
    IOService *service = IOService::waitForMatchingService(matching);
    SEPHibernatorService *typed = OSDynamicCast(SEPHibernatorService, service);
    if (!typed) {
        panic("SEPHibernator: matching service unavailable or has wrong class");
    }
    SEPHibernator *result = typed->hibernator;
    matching->release();
    service->release();
    return result;
}

static_assert(sizeof(OSObject) == 0x10, "unexpected OSObject ABI");
static_assert(sizeof(SEPHibernator) == 0x18, "unexpected SEPHibernator ABI");
static_assert(sizeof(SEPHibernatorService) == 0x90, "unexpected SEPHibernatorService ABI");
static_assert(__builtin_offsetof(SEPHibernatorService, hibernator) == 0x88,
              "unexpected hibernator member offset");
