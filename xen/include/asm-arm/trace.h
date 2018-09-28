#ifndef __ASM_TRACE_H__
#define __ASM_TRACE_H__

/* Trace events for ARM IRQs */

#define TRC_AIRQ                    (TRC_HW_IRQ + 0x800)
#define TRC_AIRQ_1                  (TRC_AIRQ + 0x1)
#define TRC_AIRQ_2                  (TRC_AIRQ + 0x2)
#define TRC_AIRQ_3                  (TRC_AIRQ + 0x3)
#define TRC_AIRQ_4                  (TRC_AIRQ + 0x4)
#define TRC_AIRQ_5                  (TRC_AIRQ + 0x5)
#define TRC_AIRQ_6                  (TRC_AIRQ + 0x6)
#define TRC_AIRQ_SPB                (TRC_AIRQ + 0x7)
#define TRC_AIRQ_SPA                (TRC_AIRQ + 0x8)
#define TRC_AIRQ_SPBU               (TRC_AIRQ + 0x9)
#define TRC_AIRQ_SPU                (TRC_AIRQ + 0xA)

#define DVCPUID(v) ((uint32_t)(((v)->domain->domain_id << 16) + (v)->vcpu_id))



#endif /* __ASM_TRACE_H__ */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
