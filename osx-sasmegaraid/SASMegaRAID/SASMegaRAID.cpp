#include "SASMegaRAID.h"
#include "Registers.h"

//#define IOPhysSize 32

OSDefineMetaClassAndStructors(SASMegaRAID, IOSCSIParallelInterfaceController)
OSDefineMetaClassAndStructors(mraid_ccbCommand, IOCommand)

bool SASMegaRAID::init (OSDictionary* dict)
{
    BaseClass::init(dict);
    //DbgPrint("IOService->init\n");
    
    fPCIDevice = NULL;
    map = NULL;
    MyWorkLoop = NULL; fInterruptSrc = NULL;
    InterruptsActivated = false;
    fMSIEnabled = false;
    ccb_inited = false;
    conf = OSDynamicCast(OSDictionary, getProperty("Settings"));
    
	/* Create an instance of PCI class from Helper Library */
	PCIHelperP = new PCIHelper<SASMegaRAID>;
    
    sc.sc_iop = IONew(mraid_iop_ops, 1);
    sc.sc_ccb_spin = NULL; sc.sc_lock = NULL;
    
    sc.sc_pcq = sc.sc_frames = sc.sc_sense = NULL;
    
    return true;
}

IOService *SASMegaRAID::probe (IOService* provider, SInt32* score)
{
    //DbgPrint("IOService->probe\n");
    return BaseClass::probe(provider, score);
}

//bool SASMegaRAID::start(IOService *provider)
bool SASMegaRAID::InitializeController(void)
{
    IOService *provider = getProvider();
    IODeviceMemory *MemDesc;
    UInt8 regbar;
    UInt32 type, barval;
    
    //BaseClass::start(provider);
    DbgPrint("super->InitializeController\n");
    
    if (!(fPCIDevice = OSDynamicCast(IOPCIDevice, provider))) {
        IOPrint("Failed to cast provider\n");
        return false;
    }

    fPCIDevice->retain();
    fPCIDevice->open(this);
    
    if(!(mpd = MatchDevice())) {
        IOPrint("Device matching failed\n");
        return false;
    }

	/* Choosing BAR register */
	regbar = (mpd->mpd_iop == MRAID_IOP_GEN2 || mpd->mpd_iop == MRAID_IOP_SKINNY) ?
        MRAID_BAR_GEN2 : MRAID_BAR;

    fPCIDevice->setBusMasterEnable(true);
    /* Figuring out mapping scheme */
    type = PCIHelperP->MappingType(this, regbar, &barval);
    switch(type) {
        case PCI_MAPREG_TYPE_IO:
            fPCIDevice->setIOEnable(true);
            break;
        case PCI_MAPREG_MEM_TYPE_32BIT_1M:
        case PCI_MAPREG_MEM_TYPE_32BIT:
            fPCIDevice->setMemoryEnable(true);

            if (!(MemDesc = IODeviceMemory::withRange(barval, MRAID_PCI_MEMSIZE))) {
                IOPrint("Memory mapping failed\n");
                return false;
            }
            
            if(MemDesc != NULL) {
                map = MemDesc->map();
                MemDesc->release();
                if(map != NULL) {
                    vAddr = (void *) map->getVirtualAddress();
                    DbgPrint("Memory mapped at virtual address %#x,"
                             " length %d\n", (UInt32)map->getVirtualAddress(),
                             (UInt32)map->getLength());
                }
                else {
                    IOPrint("Can't map controller PCI space.\n");
                    return false;
                }
            }
            break;
        case PCI_MAPREG_MEM_TYPE_64BIT:
            /*IOPrint("Only PCI-E cards are supported.\n");
             return false;*/
            
            fPCIDevice->setMemoryEnable(true);
            
            /* Rework: Mapping with 64-bit address. */
            MemDesc = IODeviceMemory::withRange((IOPhysicalAddress64) barval >> 32, MRAID_PCI_MEMSIZE);
            if(MemDesc != NULL) {
                map = MemDesc->map();
                MemDesc->release();
                if(map != NULL) {
                    vAddr = (void *) map->getVirtualAddress();
                    DbgPrint("Memory mapped at bus address %d, virtual address %#x,"
                             " length %d\n", (UInt32)map->getPhysicalAddress(),
                             (UInt32)map->getVirtualAddress(),
                             (UInt32)map->getLength());
                }
                else {
                    IOPrint("Can't map controller PCI space.\n");
                    return false;
                }
            }
            break;
        default:
            DbgPrint("Can't find out mapping scheme.\n");
            return false;
    }
    OSBoolean *sPreferMSI = conf ? OSDynamicCast(OSBoolean, conf->getObject("PreferMSI")) : NULL;
    bool PreferMSI = true;
    if (sPreferMSI) PreferMSI = sPreferMSI->isTrue();
    if(!PCIHelperP->CreateDeviceInterrupt(this, provider, PreferMSI, &SASMegaRAID::interruptHandler,
                                          &SASMegaRAID::interruptFilter))
        return false;
    
    if(!Attach()) {
        IOPrint("Can't attach device.\n");
        return false;
    }

    return true;
}

//void SASMegaRAID::stop(IOService *provider)
void SASMegaRAID::TerminateController(void)
{
    /*DbgPrint("super->TerminateController\n");
    BaseClass::stop(provider);*/
}

void SASMegaRAID::free(void)
{
    mraid_ccbCommand *command;
    
    DbgPrint("IOService->free\n");
    
    if (fPCIDevice) {
        fPCIDevice->close(this);
        fPCIDevice->release();
    }
    if(map) map->release();
    if (fInterruptSrc) {
        if (MyWorkLoop)
            MyWorkLoop->removeEventSource(fInterruptSrc);
        if (fInterruptSrc) fInterruptSrc->release();
    }
    if (ccb_inited)
        for (int i = 0; i < sc.sc_max_cmds; i++)
        {
            if ((command = (mraid_ccbCommand *) ccbCommandPool->getCommand(false)))
                command->release();
        }
    if (ccbCommandPool) ccbCommandPool->release();
    
    /* Helper Library is not inherited from OSObject */
    /*PCIHelperP->release();*/
    delete PCIHelperP;
    if (sc.sc_iop) IODelete(sc.sc_iop, mraid_iop_ops, 1);
    if(sc.sc_ccb_spin) {
        /*IOSimpleLockUnlock(sc.sc_ccb_spin);*/ IOSimpleLockFree(sc.sc_ccb_spin);
    }
    if (sc.sc_lock) {
        /*IORWLockUnlock(sc.sc_lock);*/ IORWLockFree(sc.sc_lock);
    }
    
    if (sc.sc_pcq) FreeMem(sc.sc_pcq);
    if (sc.sc_frames) FreeMem(sc.sc_frames);
    if (sc.sc_sense) FreeMem(sc.sc_sense);
    
    BaseClass::free();
}

/* */

bool SASMegaRAID::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    /* Check by which device the interrupt line is occupied (mine or other) */
    return mraid_my_intr();
}
void SASMegaRAID::interruptHandler(OSObject *owner, void *src, IOService *nub, int count)
{
    return;
}

/* */

const mraid_pci_device* SASMegaRAID::MatchDevice()
{
	using mraid_structs::mraid_pci_devices;
    
	const mraid_pci_device *mpd;
	UInt16 VendorId, DeviceId;
	
	VendorId = fPCIDevice->configRead16(kIOPCIConfigVendorID);
	DeviceId = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
	
	for (int i = 0; i < nitems(mraid_pci_devices); i++) {
		mpd = &mraid_pci_devices[i];
		
		if (mpd->mpd_vendor == VendorId &&
			mpd->mpd_product == DeviceId)
            return mpd;
	}
    
	return NULL;
}

bool SASMegaRAID::Probe()
{
    bool uiop = (mpd->mpd_iop == MRAID_IOP_XSCALE || mpd->mpd_iop == MRAID_IOP_PPC || mpd->mpd_iop == MRAID_IOP_GEN2
                 || mpd->mpd_iop == MRAID_IOP_SKINNY);
    if(sc.sc_iop->is_set() && !uiop) {
        IOPrint("%s: Unknown IOP %d. The driver will unload.\n", __FUNCTION__, mpd->mpd_iop);
        return false;
    }
    
    return true;
}

bool SASMegaRAID::Attach()
{
    UInt32 status, max_sgl, frames;
    
    DbgPrint("%s\n", __FUNCTION__);
    
    sc.sc_iop->init(mpd->mpd_iop);
    if (!this->Probe())
        return false;
    
    /* Set firmware to working state */
    if(!Transition_Firmware())
        return false;
    
    if(!(ccbCommandPool = IOCommandPool::withWorkLoop(MyWorkLoop))) {
        DbgPrint("Can't init command pool.\n");
        return false;
    }
    
    sc.sc_ccb_spin = IOSimpleLockAlloc(); sc.sc_lock = IORWLockAlloc();
    
    /* Get constraints forming frames pool contiguous memory */
    status = mraid_fw_state();
    sc.sc_max_cmds = status & MRAID_STATE_MAXCMD_MASK;
    max_sgl = (status & MRAID_STATE_MAXSGL_MASK) >> 16;
    /* FW can accept 64-bit SGLs */
    if(IOPhysSize == 64) {
        sc.sc_max_sgl = min(max_sgl, (128 * 1024) / PAGE_SIZE + 1);
        sc.sc_sgl_size = sizeof(mraid_sg64);
        sc.sc_sgl_flags = MRAID_FRAME_SGL64;
    } else {
        sc.sc_max_sgl = max_sgl;
        sc.sc_sgl_size = sizeof(mraid_sg32);
        sc.sc_sgl_flags = MRAID_FRAME_SGL32;
    }
    DbgPrint("DMA: %d-bit, max commands: %u, max SGL count: %u\n", IOPhysSize, sc.sc_max_cmds,
              sc.sc_max_sgl);
    
    /* Allocate united mem for reply queue & producer-consumer */
    if (!(sc.sc_pcq = AllocMem(sizeof(UInt32) /* Context size */
                               * sc.sc_max_cmds +
                               /* FW = producer of completed cmds
                                  driver = consumer */
                               sizeof(mraid_prod_cons)))) {
        IOPrint("Unable to allocate reply queue memory\n");
        return false;
    }

    /* Command frames memory */
    frames = (sc.sc_sgl_size * sc.sc_max_sgl + MRAID_FRAME_SIZE - 1) / MRAID_FRAME_SIZE;
    /* Extra frame for MFI_CMD */
    frames++;
    sc.sc_frames_size = frames * MRAID_FRAME_SIZE;
    if (!(sc.sc_frames = AllocMem(sc.sc_frames_size * sc.sc_max_cmds))) {
        IOPrint("Unable to allocate frame memory\n");
        return false;
    }
    /* Rework: handle this case */
    if (MRAID_DVA(sc.sc_frames) & 0x3f) {
        IOPrint("Wrong frame alignment. Addr %#llx\n", MRAID_DVA(sc.sc_frames));
        return false;
    }
    
    /* Frame sense memory */
    if (!(sc.sc_sense = AllocMem(sc.sc_max_cmds * MRAID_SENSE_SIZE))) {
        IOPrint("Unable to allocate sense memory\n");
        return false;
    }
    
    /* Init pool of commands */
    Initccb();

    /* Init firmware with all pointers */
    if (!Initialize_Firmware()) {
        IOPrint("Unable to init firmware\n");
        return false;
    }
    
    if (!GetInfo()) {
        IOPrint("Unable to get controller info\n");
        return false;
    }
    
    return true;
}

mraid_mem *SASMegaRAID::AllocMem(vm_size_t size)
{
#if 0
    IOReturn st = kIOReturnSuccess;
    UInt64 offset = 0;
    UInt32 numSeg;
#endif
    IOByteCount length;
    
    mraid_mem *mm;
    
    if (!(mm = IONew(mraid_mem, 1)))
        return NULL;
    
    /* Below 4gb for fast access */
    if (!(mm->bmd = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIOMemoryPhysicallyContiguous, size, PAGE_SIZE))) {
        goto free;
    }
    
    mm->bmd->prepare();
    mm->vaddr = (IOVirtualAddress) mm->bmd->getBytesNoCopy();
    mm->paddr = mm->bmd->getPhysicalSegment(0, &length);
    /* Zero the whole region for easy */
    memset((void *) mm->vaddr, 0, size);
    
    return mm;
#if 0
    if (!(mm->cmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kMapped, size, PAGE_SIZE)))
        goto bmd_free;
    
    if (mm->cmd->setMemoryDescriptor(mm->bmd /*, autoPrepare = true*/) != kIOReturnSuccess)
        goto cmd_free;
    
    while ((st == kIOReturnSuccess) && (offset < mm->bmd->getLength()))
    {
        numSeg = 1;
        
        st = mm->cmd->gen32IOVMSegments(&offset, &mm->segment, &numSeg);
        DbgPrint("gen32IOVMSegments(err = %d) paddr %#x, len %d, nsegs %d\n",
              st, mm->segment.fIOVMAddr, mm->segment.fLength, numSeg);
    }
    if (st == kIOReturnSuccess) {
        mm->map = mm->bmd->map();
        return mm;
    }
    
cmd_free:
    mm->cmd->clearMemoryDescriptor(/*autoComplete = true*/);
    mm->cmd->release();
bmd_free:
    mm->bmd->release();
#endif
    
free:
    IODelete(mm, mraid_mem, 1);
    
    return NULL;
}
void SASMegaRAID::FreeMem(mraid_mem *mm)
{
#if 0
    mm->map->release();
    mm->cmd->clearMemoryDescriptor(/*autoComplete = true*/);
    mm->cmd->release();
#endif
    mm->bmd->complete();
    mm->bmd->release();
    IODelete(mm, mraid_mem, 1);
    mm = NULL;
}

#if 0
bool SASMegaRAID::GenerateSegments(mraid_ccbCommand *ccb)
{
    IOReturn st = kIOReturnSuccess;
    UInt64 offset = 0;

    if (!(ccb->s.ccb_sglmem.cmd = IODMACommand::withSpecification(IOPhysSize == 64 ?
                                                                      kIODMACommandOutputHost64:
                                                                      kIODMACommandOutputHost32,
                                                                      IOPhysSize, MAXPHYS,
                                                                      IODMACommand::kMapped, MAXPHYS)))
        return false;
    
    /* TO-DO: Set kIOMemoryMapperNone or ~kIOMemoryMapperNone */
    if (ccb->s.ccb_sglmem.cmd->setMemoryDescriptor(ccb->s.ccb_sglmem.bmd, /* autoPrepare */ false) != kIOReturnSuccess)
        return false;
    ccb->s.ccb_sglmem.cmd->prepare(0, 0, false, /*synchronize*/ false);
    
    ccb->s.ccb_sglmem.numSeg = sc.sc_max_sgl;
    while ((st == kIOReturnSuccess) && (offset < ccb->s.ccb_sglmem.bmd->getLength()))
    {
        /* TO-DO: gen64IOVMSegments */
        ccb->s.ccb_sglmem.segments = IONew(IODMACommand::Segment32, sc.sc_max_sgl);
        
        st = ccb->s.ccb_sglmem.cmd->gen32IOVMSegments(&offset, &ccb->s.ccb_sglmem.segments[0], &ccb->s.ccb_sglmem.numSeg);
        if (ccb->s.ccb_sglmem.numSeg > 1)
            return false;
    }
    if (st != kIOReturnSuccess)
        return false;
    
    DbgPrint("gen32IOVMSegments: nseg %d, perseg %d, totlen %lld\n", ccb->s.ccb_sglmem.numSeg,
                ccb->s.ccb_sglmem.segments[0].fLength, ccb->s.ccb_sglmem.bmd->getLength());
    return true;
}
#endif

void SASMegaRAID::Initccb()
{
    mraid_ccbCommand *ccb;
    
    DbgPrint("%s\n", __FUNCTION__);
    
    /* Reverse init since IOCommandPool is LIFO */
    for (int i = sc.sc_max_cmds - 1; i >= 0; i--) {
        ccb = mraid_ccbCommand::NewCommand();

        /* Pick i'th frame & i'th sense */
        
        ccb->s.ccb_frame = (mraid_frame *) ((char *) MRAID_KVA(sc.sc_frames) + sc.sc_frames_size * i);
        ccb->s.ccb_pframe = MRAID_DVA(sc.sc_frames) + sc.sc_frames_size * i;
        //ccb->s.ccb_pframe_offset = sc.sc_frames_size * i;
        ccb->s.ccb_frame->mrr_header.mrh_context = i;
        
        ccb->s.ccb_sense = (mraid_sense *) ((char *) MRAID_KVA(sc.sc_sense) + MRAID_SENSE_SIZE * i);
        ccb->s.ccb_psense = MRAID_DVA(sc.sc_sense) + MRAID_SENSE_SIZE * i;

        /*DbgPrint(MRAID_D_CCB, "ccb(%d) frame: %p (%lx) sense: %p (%lx)\n",
                  cb->s.ccb_frame->mrr_header.mrh_context, ccb->ccb_frame, ccb->ccb_pframe, ccb->ccb_sense, ccb->ccb_psense);*/
        
        Putccb(ccb);
    }
    
    ccb_inited = true;
}
void SASMegaRAID::Putccb(mraid_ccbCommand *ccb)
{
    ccb->scrubCommand();
    
    IOSimpleLockLock(sc.sc_ccb_spin);
    ccbCommandPool->returnCommand(ccb);
    IOSimpleLockUnlock(sc.sc_ccb_spin);
}
mraid_ccbCommand *SASMegaRAID::Getccb()
{
    mraid_ccbCommand *ccb;
    
    IOSimpleLockLock(sc.sc_ccb_spin);
    ccb = (mraid_ccbCommand *) ccbCommandPool->getCommand(true);
    IOSimpleLockUnlock(sc.sc_ccb_spin);
    
    return ccb;
}

bool SASMegaRAID::Transition_Firmware()
{
    UInt32 fw_state, cur_state;
    int max_wait;
    
    fw_state = mraid_fw_state() & MRAID_STATE_MASK;
    
    DbgPrint("%s: Firmware state %#x\n", __FUNCTION__, fw_state);
    
    while(fw_state != MRAID_STATE_READY) {
        DbgPrint("Waiting for firmware to become ready\n");
        cur_state = fw_state;
        switch(fw_state) {
            case MRAID_STATE_FAULT:
                IOPrint("Firmware fault\n");
                return false;
            case MRAID_STATE_WAIT_HANDSHAKE:
                MRAID_Write(MRAID_IDB, MRAID_INIT_CLEAR_HANDSHAKE);
                max_wait = 2;
                break;
            case MRAID_STATE_OPERATIONAL:
                MRAID_Write(MRAID_IDB, MRAID_INIT_READY);
                max_wait = 10;
                break;
            case MRAID_STATE_UNDEFINED:
            case MRAID_STATE_BB_INIT:
                max_wait = 2;
                break;
            case MRAID_STATE_FW_INIT:
            case MRAID_STATE_DEVICE_SCAN:
            case MRAID_STATE_FLUSH_CACHE:
                max_wait = 20;
                break;
            default:
                IOPrint("Unknown firmware state\n");
                return false;
        }
        for(int i = 0; i < (max_wait * 10); i++) {
            fw_state = mraid_fw_state() & MRAID_STATE_MASK;
            if(fw_state == cur_state)
                IOSleep(100);
            else break;
        }
        if(fw_state == cur_state) {
            IOPrint("Firmware stuck in state: %#x\n", fw_state);
            return false;
        }
    }
    
    return true;
}

void mraid_empty_done(mraid_ccbCommand *)
{
    /* ;) */
    __asm__ volatile("nop");
}

bool SASMegaRAID::Initialize_Firmware()
{
    bool res = true;
    mraid_ccbCommand* ccb;
    mraid_init_frame *init;
    mraid_init_qinfo *qinfo;
    
    DbgPrint("%s\n", __FUNCTION__);
    
    ccb = Getccb();
    
    init = &ccb->s.ccb_frame->mrr_init;
    /* Skip header */
    qinfo = (mraid_init_qinfo *)((UInt8 *) init + MRAID_FRAME_SIZE);
    
    memset(qinfo, 0, sizeof(mraid_init_qinfo));
    qinfo->miq_rq_entries = htole32(sc.sc_max_cmds + 1);
    
    qinfo->miq_rq_addr = htole64(MRAID_DVA(sc.sc_pcq) + offsetof(mraid_prod_cons, mpc_reply_q));
	qinfo->miq_pi_addr = htole64(MRAID_DVA(sc.sc_pcq) + offsetof(mraid_prod_cons, mpc_producer));
	qinfo->miq_ci_addr = htole64(MRAID_DVA(sc.sc_pcq) + offsetof(mraid_prod_cons, mpc_consumer));
    
    init->mif_header.mrh_cmd = MRAID_CMD_INIT;
    init->mif_header.mrh_data_len = sizeof(mraid_init_qinfo);
    init->mif_qinfo_new_addr = htole64(ccb->s.ccb_pframe + MRAID_FRAME_SIZE);
    
    /*DbgPrint("Entries: %#x, rq: %#llx, pi: %#llx, ci: %#llx\n", qinfo->miq_rq_entries,
             qinfo->miq_rq_addr, qinfo->miq_pi_addr, qinfo->miq_ci_addr);*/
    
    //sc.sc_pcq->cmd->synchronize(kIODirectionInOut);
    
    ccb->s.ccb_done = mraid_empty_done;
    MRAID_Poll(ccb);
    if (init->mif_header.mrh_cmd_status != MRAID_STAT_OK)
        res = false;
    
done:
    Putccb(ccb);
    
    return res;
}

bool SASMegaRAID::GetInfo()
{
    DbgPrint("%s\n", __FUNCTION__);
    
    if (!Management(MRAID_DCMD_CTRL_GET_INFO, MRAID_DATA_IN, sizeof(sc.sc_info), &sc.sc_info, NULL))
        return false;

    IOPrint("%d\n", sc.sc_info.mci_image_component_count);
#if 0
    int i;
	for (i = 0; i < sc.sc_info.mci_image_component_count; i++) {
		IOPrint("Active FW %s Version %s date %s time %s\n",
               sc.sc_info.mci_image_component[i].mic_name,
               sc.sc_info.mci_image_component[i].mic_version,
               sc.sc_info.mci_image_component[i].mic_build_date,
               sc.sc_info.mci_image_component[i].mic_build_time);
	}
#endif
    
    return true;
}

bool SASMegaRAID::Management(UInt32 opc, UInt32 dir, UInt32 len, void *buf, UInt8 *mbox)
{
    mraid_ccbCommand* ccb;
    bool res;
    
    ccb = Getccb();    
    res = Do_Management(ccb, opc, dir, len, buf, mbox);
    Putccb(ccb);
    
    return res;
}
bool SASMegaRAID::Do_Management(mraid_ccbCommand *ccb, UInt32 opc, UInt32 dir, UInt32 len, void *buf, UInt8 *mbox)
{
    mraid_dcmd_frame *dcmd;
    
    IOBufferMemoryDescriptor *bmd;
    //IOMemoryMap *map;
    IOVirtualAddress addr;
    
    DbgPrint("%s: ccb_num: %d, opcode: %#x\n", __FUNCTION__, ccb->s.ccb_frame->mrr_header.mrh_context, opc);
    
    /* Support 64-bit DMA */
    if (!(bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIOMemoryPhysicallyContiguous |
                                                                                kIOMapInhibitCache /* Disable caching */
                                                                 ,len, IOPhysSize == 64 ? 0xFFFFFFFFFFFFFFFFULL
                                                                 : 0x00000000FFFFF000ULL)))
        return false;
    
    bmd->prepare();
    addr = (IOVirtualAddress) bmd->getBytesNoCopy();
    memset((void *) addr, 0, len);
    
    dcmd = &ccb->s.ccb_frame->mrr_dcmd;
    memset(dcmd->mdf_mbox, 0, MRAID_MBOX_SIZE);
    dcmd->mdf_header.mrh_cmd = MRAID_CMD_DCMD;
    dcmd->mdf_header.mrh_timeout = 0;
    
    dcmd->mdf_opcode = opc;
    dcmd->mdf_header.mrh_data_len = 0;
    ccb->s.ccb_direction = dir;
    
    ccb->s.ccb_frame_size = MRAID_DCMD_FRAME_SIZE;
    
    /* Additional opcodes */
    if (mbox)
        memcpy(dcmd->mdf_mbox, mbox, MRAID_MBOX_SIZE);
    
    if (dir != MRAID_DATA_NONE) {
        if (dir == MRAID_DATA_OUT)
            bcopy(buf, (void *) addr, len);
        dcmd->mdf_header.mrh_data_len = len;
        
        ccb->s.ccb_sglmem.bmd = bmd;
        //ccb->s.ccb_sglmem.map = map;
        ccb->s.ccb_sglmem.len = len;
        
        ccb->s.ccb_sgl = &dcmd->mdf_sgl;
        
        if (!CreateSGL(ccb))
            return false;
    }

    if (!InterruptsActivated) {
        ccb->s.ccb_done = mraid_empty_done;
        MRAID_Poll(ccb);
    } else {
        ccb->s.ccb_lock.holder = IOLockAlloc();
        MRAID_Exec(ccb);
        IOLockFree(ccb->s.ccb_lock.holder);
    }
    if (dcmd->mdf_header.mrh_cmd_status != MRAID_STAT_OK)
        return false;

    if (ccb->s.ccb_direction == MRAID_DATA_IN)
        bcopy((void *) addr, buf, len);
    
    FreeSGL(&ccb->s.ccb_sglmem);
    
    return true;
}

bool SASMegaRAID::CreateSGL(mraid_ccbCommand *ccb)
{
    mraid_frame_header *hdr = &ccb->s.ccb_frame->mrr_header;
    mraid_sgl *sgl;
    //IODMACommand::Segment32 *sgd;
    IOByteCount length;
    
    DbgPrint("%s\n", __FUNCTION__);
    
    ccb->s.ccb_sglmem.paddr = ccb->s.ccb_sglmem.bmd->getPhysicalSegment(0, &length);
#if 0
    if (!GenerateSegments(ccb)) {
        IOPrint("Unable to generate segments\n");
        return false;
    }
#endif
    
    sgl = ccb->s.ccb_sgl;
#if 0
    sgd = ccb->s.ccb_sglmem.segments;
    for (int i = 0; i < ccb->s.ccb_sglmem.numSeg; i++) {
#endif
        if (IOPhysSize == 64) {
            sgl->sg64[0].addr = htole64(ccb->s.ccb_sglmem.paddr);
            sgl->sg64[0].len = htole32(ccb->s.ccb_sglmem.len);
        } else {
            sgl->sg32[0].addr = htole32(ccb->s.ccb_sglmem.paddr);
            sgl->sg32[0].len = htole32(ccb->s.ccb_sglmem.len);
        }
        DbgPrint("SGL paddr: %#llx\n", ccb->s.ccb_sglmem.paddr);
    //}
    
    if (ccb->s.ccb_direction == MRAID_DATA_IN) {
        hdr->mrh_flags |= MRAID_FRAME_DIR_READ;
        //ccb->s.ccb_sglmem.cmd->synchronize(kIODirectionIn);
    } else {
        hdr->mrh_flags |= MRAID_FRAME_DIR_WRITE;
        //ccb->s.ccb_sglmem.cmd->synchronize(kIODirectionOut);
    }
    
    hdr->mrh_flags |= sc.sc_sgl_flags;
    hdr->mrh_sg_count = 1; //ccb->s.ccb_sglmem.numSeg
    ccb->s.ccb_frame_size += sc.sc_sgl_size; //* ccb->s.ccb_sglmem.numSeg
    ccb->s.ccb_extra_frames = (ccb->s.ccb_frame_size - 1) / MRAID_FRAME_SIZE;
    
    DbgPrint("frame_size: %d extra_frames: %d\n",
             ccb->s.ccb_frame_size, ccb->s.ccb_extra_frames);
    
    return true;
}

UInt32 SASMegaRAID::MRAID_Read(UInt8 offset)
{
    UInt32 data;
    /*if(MemDesc.readBytes(offset, &data, 4) != 4) {
     DbgPrint("%s[%p]::Read(): Unsuccessfull.\n", getName(), this);
     return(0);
     }*/
    data = OSReadLittleInt32(vAddr, offset);
    DbgPrint("%s: offset %#x data 0x08%x\n", __FUNCTION__, offset, data);
    
    return data;
}
/*bool*/
void SASMegaRAID::MRAID_Write(UInt8 offset, uint32_t data)
{
    DbgPrint("%s: offset %#x data 0x08%x\n", __FUNCTION__, offset, data);
    OSWriteLittleInt32(vAddr, offset, data);
    OSSynchronizeIO();
    
    /*if(MemDesc.writeBytes(offset, &data, 4) != 4) {
     DbgPrint("%s[%p]::Write(): Unsuccessfull.\n", getName(), this);
     return FALSE;
     }
     
     return true;*/
}

void SASMegaRAID::MRAID_Poll(mraid_ccbCommand *ccb)
{
    mraid_frame_header *hdr;
    int cycles = 0;
    
    DbgPrint("%s\n", __FUNCTION__);
    
    hdr = &ccb->s.ccb_frame->mrr_header;
    hdr->mrh_cmd_status = 0xff;
    hdr->mrh_flags |= MRAID_FRAME_DONT_POST_IN_REPLY_QUEUE;
    
    mraid_post(ccb);
    
    while (1) {
        IOSleep(10);
        
        //sc.sc_frames->cmd->synchronize(kIODirectionInOut);
        
        if (hdr->mrh_cmd_status != 0xff)
            break;
        
        if (cycles++ > 500) {
            IOPrint("ccb timeout\n");
            break;
        }
        
        //sc.sc_frames->cmd->synchronize(kIODirectionInOut);
    }

    /*if (ccb->s.ccb_sglmem.len > 0)
        ccb->s.ccb_sglmem.cmd->synchronize((ccb->s.ccb_direction & MRAID_DATA_IN) ?
                                         kIODirectionIn : kIODirectionOut);*/
    
    ccb->s.ccb_done(ccb);
}

/* Interrupt driven */
void mraid_exec_done(mraid_ccbCommand *ccb)
{
    IOLockLock(ccb->s.ccb_lock.holder);
    ccb->s.ccb_lock.event = true;
    IOLockWakeup(ccb->s.ccb_lock.holder, &ccb->s.ccb_lock.event, true);
    IOLockUnlock(ccb->s.ccb_lock.holder);
}
void SASMegaRAID::MRAID_Exec(mraid_ccbCommand *ccb)
{
#if defined(DEBUG)
    if (ccb->s.ccb_lock.event || ccb->s.ccb_done)
        DbgPrint("%s Warning: event or done set\n", __FUNCTION__);
#endif
    ccb->s.ccb_done = mraid_exec_done;
    mraid_post(ccb);
    
    IOLockLock(ccb->s.ccb_lock.holder);
    IOLockSleep(ccb->s.ccb_lock.holder, &ccb->s.ccb_lock.event, THREAD_INTERRUPTIBLE);
    ccb->s.ccb_lock.event = false;
    IOLockUnlock(ccb->s.ccb_lock.holder);
}
/* */

bool SASMegaRAID::mraid_xscale_intr()
{
    UInt32 Status;
    
    Status = MRAID_Read(MRAID_OSTS);
    if(!(Status & MRAID_OSTS_INTR_VALID))
        return false;
    
    /* Write status back to acknowledge interrupt */
    /*if(!MRAID_Write(MRAID_OSTS, Status))
     return false;*/
    MRAID_Write(MRAID_OSTS, Status);
    
    return true;
}
UInt32 SASMegaRAID::mraid_xscale_fw_state()
{
    return MRAID_Read(MRAID_OMSG0);
}
void SASMegaRAID::mraid_xscale_post(mraid_ccbCommand *ccb)
{
    MRAID_Write(MRAID_IQP, (ccb->s.ccb_pframe >> 3) | ccb->s.ccb_extra_frames);
}
bool SASMegaRAID::mraid_ppc_intr()
{
    UInt32 Status;
    
    Status = MRAID_Read(MRAID_OSTS);
    if(!(Status & MRAID_OSTS_PPC_INTR_VALID))
        return false;
    
    /* Write status back to acknowledge interrupt */
    /*if(!MRAID_Write(MRAID_ODC, Status))
     return false;*/
    MRAID_Write(MRAID_ODC, Status);
    
    return true;
}
UInt32 SASMegaRAID::mraid_ppc_fw_state()
{
    return(MRAID_Read(MRAID_OSP));
}
void SASMegaRAID::mraid_ppc_post(mraid_ccbCommand *ccb)
{
    MRAID_Write(MRAID_IQP, 0x1 | ccb->s.ccb_pframe | (ccb->s.ccb_extra_frames << 1));
}
bool SASMegaRAID::mraid_gen2_intr()
{
    UInt32 Status;
    
    Status = MRAID_Read(MRAID_OSTS);
    if(!(Status & MRAID_OSTS_GEN2_INTR_VALID))
        return false;
    
    /* Write status back to acknowledge interrupt */
    /*if(!MRAID_Write(MRAID_ODC, Status))
     return false;*/
    MRAID_Write(MRAID_ODC, Status);
    
    return true;
}
UInt32 SASMegaRAID::mraid_gen2_fw_state()
{
    return(MRAID_Read(MRAID_OSP));
}
bool SASMegaRAID::mraid_skinny_intr()
{
    UInt32 Status;
    
    Status = MRAID_Read(MRAID_OSTS);
    if(!(Status & MRAID_OSTS_SKINNY_INTR_VALID))
        return false;
    
    /* Write status back to acknowledge interrupt */
    /*if(!MRAID_Write(MRAID_ODC, Status))
     return false;*/
    MRAID_Write(MRAID_ODC, Status);
    
    return true;
}
UInt32 SASMegaRAID::mraid_skinny_fw_state()
{
    return MRAID_Read(MRAID_OSP);
}
void SASMegaRAID::mraid_skinny_post(mraid_ccbCommand *ccb)
{
    MRAID_Write(MRAID_IQPL, 0x1 | ccb->s.ccb_pframe | (ccb->s.ccb_extra_frames << 1));
    MRAID_Write(MRAID_IQPH, 0x00000000);
}