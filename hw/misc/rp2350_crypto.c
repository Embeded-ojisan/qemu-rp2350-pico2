/*
 * RP2350 TRNG and SHA-256 (RP-008373 sections 12.12 and 12.13)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/guest-random.h"
#include "hw/core/irq.h"
#include "hw/misc/rp2350_crypto.h"
#include "migration/vmstate.h"

#define TRNG_IMR        0x100
#define TRNG_ISR        0x104
#define TRNG_ICR        0x108
#define TRNG_CONFIG     0x10c
#define TRNG_VALID      0x110
#define TRNG_EHR0       0x114
#define TRNG_EHR5       0x128
#define TRNG_ENABLE     0x12c
#define TRNG_COUNT      0x130
#define TRNG_DEBUG      0x138
#define TRNG_RESET      0x140

static void trng_update_irq(RP2350TRNGState *s)
{
    qemu_set_irq(s->irq, !!(s->isr & ~s->imr));
}

static void trng_sample(RP2350TRNGState *s)
{
    /* shortcut: instant host entropy, without oscillator or health tests. */
    qemu_guest_getrandom_nofail(s->entropy, sizeof(s->entropy));
    s->isr |= 1;
    trng_update_irq(s);
}

static void trng_reset(Object *obj, ResetType type)
{
    RP2350TRNGState *s = RP2350_TRNG(obj);

    s->imr = 0xf;
    s->isr = s->config = s->debug = 0;
    s->sample_count = 0xffff;
    s->enabled = false;
    memset(s->entropy, 0, sizeof(s->entropy));
    trng_update_irq(s);
}

static uint64_t trng_read(void *opaque, hwaddr offset, unsigned size)
{
    RP2350TRNGState *s = opaque;

    switch (offset) {
    case TRNG_IMR:
        return s->imr;
    case TRNG_ISR:
        return s->isr;
    case TRNG_VALID:
    case TRNG_ENABLE:
        return s->enabled;
    case TRNG_CONFIG:
        return s->config;
    case TRNG_COUNT:
        return s->sample_count;
    case TRNG_DEBUG:
        return s->debug;
    case TRNG_EHR0 ... TRNG_EHR5: {
        uint32_t value = s->entropy[(offset - TRNG_EHR0) / 4];

        if (offset == TRNG_EHR5 && s->enabled) {
            trng_sample(s);
        }
        return value;
    }
    default:
        return 0;
    }
}

static void trng_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    RP2350TRNGState *s = opaque;

    switch (offset) {
    case TRNG_IMR:
        s->imr = value & 0xf;
        trng_update_irq(s);
        break;
    case TRNG_ICR:
        s->isr &= ~value;
        trng_update_irq(s);
        break;
    case TRNG_CONFIG:
        s->config = value & 3;
        break;
    case TRNG_ENABLE:
        if ((value & 1) && !s->enabled) {
            trng_sample(s);
        }
        s->enabled = value & 1;
        break;
    case TRNG_COUNT:
        s->sample_count = value;
        break;
    case TRNG_DEBUG:
        s->debug = value & 0xe;
        break;
    case TRNG_RESET:
        if (value & 1) {
            trng_reset(OBJECT(s), RESET_TYPE_COLD);
        }
        break;
    }
}

static const MemoryRegionOps trng_ops = {
    .read = trng_read,
    .write = trng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void trng_init(Object *obj)
{
    RP2350TRNGState *s = RP2350_TRNG(obj);

    memory_region_init_io(&s->iomem, obj, &trng_ops, s,
                          TYPE_RP2350_TRNG, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_trng = {
    .name = TYPE_RP2350_TRNG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(imr, RP2350TRNGState),
        VMSTATE_UINT32(isr, RP2350TRNGState),
        VMSTATE_UINT32(config, RP2350TRNGState),
        VMSTATE_UINT32(sample_count, RP2350TRNGState),
        VMSTATE_UINT32(debug, RP2350TRNGState),
        VMSTATE_UINT32_ARRAY(entropy, RP2350TRNGState, 6),
        VMSTATE_BOOL(enabled, RP2350TRNGState),
        VMSTATE_END_OF_LIST()
    },
};

static void trng_class_init(ObjectClass *klass, const void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = trng_reset;
    DEVICE_CLASS(klass)->vmsd = &vmstate_trng;
}

#define SHA_CSR          0x00
#define SHA_WDATA        0x04
#define SHA_SUM0         0x08
#define SHA_SUM7         0x24
#define SHA_START        BIT(0)
#define SHA_WDATA_RDY    BIT(1)
#define SHA_SUM_VLD      BIT(2)
#define SHA_BSWAP        BIT(12)
#define SHA_CSR_RW       (SHA_BSWAP | (3 << 8))
#define SHA_CSR_RESET    (SHA_BSWAP | (2 << 8))

static const uint32_t sha_initial[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

/* FIPS 180-4, section 6.2.2. The guest writes a padded 512-bit block. */
static void sha_compress(RP2350SHA256State *s)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64];
    uint32_t a = s->digest[0], b = s->digest[1], c = s->digest[2];
    uint32_t d = s->digest[3], e = s->digest[4], f = s->digest[5];
    uint32_t g = s->digest[6], h = s->digest[7];

    for (unsigned i = 0; i < 16; i++) {
        const uint8_t *word = &s->block[i * 4];

        w[i] = s->csr & SHA_BSWAP ? ldl_be_p(word) : ldl_le_p(word);
    }
    for (unsigned i = 16; i < ARRAY_SIZE(w); i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        uint32_t sigma0 = ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3);
        uint32_t sigma1 = ror32(y, 17) ^ ror32(y, 19) ^ (y >> 10);

        w[i] = w[i - 16] + sigma0 + w[i - 7] + sigma1;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++) {
        uint32_t sum0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t sum1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t1 = h + sum1 + choose + k[i] + w[i];
        uint32_t t2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->digest[0] += a;
    s->digest[1] += b;
    s->digest[2] += c;
    s->digest[3] += d;
    s->digest[4] += e;
    s->digest[5] += f;
    s->digest[6] += g;
    s->digest[7] += h;
}

static uint64_t sha_read(void *opaque, hwaddr offset, unsigned size)
{
    RP2350SHA256State *s = opaque;

    switch (offset) {
    case SHA_CSR:
        return s->csr | SHA_WDATA_RDY | (s->used ? 0 : SHA_SUM_VLD);
    case SHA_SUM0 ... SHA_SUM7:
        return s->digest[(offset - SHA_SUM0) / 4];
    default:
        return 0;
    }
}

static void sha_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RP2350SHA256State *s = opaque;

    if (offset == SHA_CSR) {
        s->csr = value & SHA_CSR_RW;
        if (value & SHA_START) {
            memcpy(s->digest, sha_initial, sizeof(s->digest));
            s->used = 0;
        }
    } else if (offset == SHA_WDATA) {
        /* shortcut: finish compression immediately, with no DMA DREQ. */
        for (unsigned i = 0; i < size; i++) {
            s->block[s->used++] = value >> (8 * i);
            if (s->used == sizeof(s->block)) {
                sha_compress(s);
                s->used = 0;
            }
        }
    }
}

static const MemoryRegionOps sha_ops = {
    .read = sha_read,
    .write = sha_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void sha_reset(Object *obj, ResetType type)
{
    RP2350SHA256State *s = RP2350_SHA256(obj);

    s->csr = SHA_CSR_RESET;
    s->used = 0;
    memset(s->digest, 0, sizeof(s->digest));
}

static void sha_init(Object *obj)
{
    RP2350SHA256State *s = RP2350_SHA256(obj);

    memory_region_init_io(&s->iomem, obj, &sha_ops, s,
                          TYPE_RP2350_SHA256, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static int sha_post_load(void *opaque, int version_id)
{
    RP2350SHA256State *s = opaque;

    return s->used < sizeof(s->block) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_sha = {
    .name = TYPE_RP2350_SHA256,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sha_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, RP2350SHA256State),
        VMSTATE_UINT32_ARRAY(digest, RP2350SHA256State, 8),
        VMSTATE_UINT8_ARRAY(block, RP2350SHA256State, 64),
        VMSTATE_UINT8(used, RP2350SHA256State),
        VMSTATE_END_OF_LIST()
    },
};

static void sha_class_init(ObjectClass *klass, const void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = sha_reset;
    DEVICE_CLASS(klass)->vmsd = &vmstate_sha;
}

static const TypeInfo rp2350_crypto_types[] = {
    {
        .name = TYPE_RP2350_TRNG,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350TRNGState),
        .instance_init = trng_init,
        .class_init = trng_class_init,
    }, {
        .name = TYPE_RP2350_SHA256,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350SHA256State),
        .instance_init = sha_init,
        .class_init = sha_class_init,
    },
};

DEFINE_TYPES(rp2350_crypto_types)
