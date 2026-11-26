/* Reconstructed 25F80 interface. Evidence and scope: SEP-HIBERNATION.md.
 * The SEP operation remains abstract and is implemented by Apple's driver.
 */
#ifndef RDXNU_SEP_HIBERNATOR_H
#define RDXNU_SEP_HIBERNATOR_H

#include <IOKit/IOService.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSPtr.h>

struct sephib_wrapped_key_t;
struct sephib_seprom_hib_payload_t;

class SEPHibernator : public OSObject {
    OSDeclareAbstractStructors(SEPHibernator);
protected:
    struct ExpansionData {};
    ExpansionData *reserved;
    bool init() override;
public:
    static OSPtr<SEPHibernator> sepHibernator();
    virtual void prepareToHibernate(sephib_wrapped_key_t *, sephib_seprom_hib_payload_t *) = 0;
    OSMetaClassDeclareReservedUnused(SEPHibernator, 0);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 1);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 2);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 3);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 4);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 5);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 6);
    OSMetaClassDeclareReservedUnused(SEPHibernator, 7);
};

class SEPHibernatorService : public IOService {
    OSDeclareDefaultStructors(SEPHibernatorService);
public:
    bool start(IOService *) override;
    SEPHibernator *hibernator;
};

#endif
