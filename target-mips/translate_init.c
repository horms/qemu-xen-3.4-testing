/*
 *  MIPS emulation for qemu: CPU initialisation routines.
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *  Copyright (c) 2007 Herve Poussineau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* CPU / CPU family specific config register values. */

/* Have config1, is MIPS32R1, uses TLB, no virtual icache,
   uncached coherency */
#define MIPS_CONFIG0                                              \
  ((1 << CP0C0_M) | (0x0 << CP0C0_K23) | (0x0 << CP0C0_KU) |      \
   (0x0 << CP0C0_AT) | (0x0 << CP0C0_AR) | (0x1 << CP0C0_MT) |    \
   (0x2 << CP0C0_K0))

/* Have config2, 16 TLB entries, 64 sets Icache, 16 bytes Icache line,
   2-way Icache, 64 sets Dcache, 16 bytes Dcache line, 2-way Dcache,
   no coprocessor2 attached, no MDMX support attached,
   no performance counters, watch registers present,
   no code compression, EJTAG present, no FPU */
#define MIPS_CONFIG1                                              \
((1 << CP0C1_M) | ((MIPS_TLB_NB - 1) << CP0C1_MMU) |              \
 (0x0 << CP0C1_IS) | (0x3 << CP0C1_IL) | (0x1 << CP0C1_IA) |      \
 (0x0 << CP0C1_DS) | (0x3 << CP0C1_DL) | (0x1 << CP0C1_DA) |      \
 (0 << CP0C1_C2) | (0 << CP0C1_MD) | (0 << CP0C1_PC) |            \
 (1 << CP0C1_WR) | (0 << CP0C1_CA) | (1 << CP0C1_EP) |            \
 (0 << CP0C1_FP))

/* Have config3, no tertiary/secondary caches implemented */
#define MIPS_CONFIG2                                              \
((1 << CP0C2_M))

/* No config4, no DSP ASE, no large physaddr,
   no external interrupt controller, no vectored interupts,
   no 1kb pages, no MT ASE, no SmartMIPS ASE, no trace logic */
#define MIPS_CONFIG3                                              \
((0 << CP0C3_M) | (0 << CP0C3_DSPP) | (0 << CP0C3_LPA) |          \
 (0 << CP0C3_VEIC) | (0 << CP0C3_VInt) | (0 << CP0C3_SP) |        \
 (0 << CP0C3_MT) | (0 << CP0C3_SM) | (0 << CP0C3_TL))

/* Define a implementation number of 1.
   Define a major version 1, minor version 0. */
#define MIPS_FCR0 ((0 << 16) | (1 << 8) | (1 << 4) | 0)


struct mips_def_t {
    const unsigned char *name;
    int32_t CP0_PRid;
    int32_t CP0_Config0;
    int32_t CP0_Config1;
    int32_t CP0_Config2;
    int32_t CP0_Config3;
    int32_t CP1_fcr0;
};

/*****************************************************************************/
/* MIPS CPU definitions */
static mips_def_t mips_defs[] =
{
#ifndef MIPS_HAS_MIPS64
    {
        .name = "4Kc",
        .CP0_PRid = 0x00018000,
        .CP0_Config0 = MIPS_CONFIG0,
        .CP0_Config1 = MIPS_CONFIG1,
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP1_fcr0 = MIPS_FCR0,
    },
    {
        .name = "4KEc",
        .CP0_PRid = 0x00018400,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR),
        .CP0_Config1 = MIPS_CONFIG1,
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP1_fcr0 = MIPS_FCR0,
    },
    {
        .name = "24Kf",
        .CP0_PRid = 0x00019300,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP1_fcr0 = MIPS_FCR0,
    },
#else
    {
        .name = "R4000",
        .CP0_PRid = 0x00000400,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP1_fcr0 = MIPS_FCR0,
    },
#endif
};

int mips_find_by_name (const unsigned char *name, mips_def_t **def)
{
    int i, ret;

    ret = -1;
    *def = NULL;
    for (i = 0; i < sizeof(mips_defs) / sizeof(mips_defs[0]); i++) {
        if (strcasecmp(name, mips_defs[i].name) == 0) {
            *def = &mips_defs[i];
            ret = 0;
            break;
        }
    }

    return ret;
}

void mips_cpu_list (FILE *f, int (*cpu_fprintf)(FILE *f, const char *fmt, ...))
{
    int i;

    for (i = 0; i < sizeof(mips_defs) / sizeof(mips_defs[0]); i++) {
        (*cpu_fprintf)(f, "MIPS '%s'\n",
                       mips_defs[i].name);
    }
}

int cpu_mips_register (CPUMIPSState *env, mips_def_t *def)
{
    if (!def)
        cpu_abort(env, "Unable to find MIPS CPU definition\n");
    env->CP0_PRid = def->CP0_PRid;
#ifdef TARGET_WORDS_BIGENDIAN
    env->CP0_Config0 = def->CP0_Config0 | (1 << CP0C0_BE);
#else
    env->CP0_Config0 = def->CP0_Config0;
#endif
    env->CP0_Config1 = def->CP0_Config1;
    env->CP0_Config2 = def->CP0_Config2;
    env->CP0_Config3 = def->CP0_Config3;
    env->fcr0 = def->CP1_fcr0;
    return 0;
}
