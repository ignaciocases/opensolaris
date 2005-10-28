/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/sysmacros.h>
#include <sys/machsystm.h>
#include <sys/cpuvar.h>
#include <sys/ddi_implfuncs.h>
#include <sys/hypervisor_api.h>
#include <px_obj.h>
#include <sys/pci_tools.h>
#include <px_tools_var.h>
#include "px_asm_4v.h"
#include "px_lib4v.h"
#include <px_tools_ext.h>

/*
 * Delay needed to have a safe environment envelop any error which could
 * surface.  The larger the number of bridges and switches, the larger the
 * number needed here.
 *
 * Note: this is a workaround until a better solution is found.  While this
 * number is high, given enough bridges and switches in the device path, this
 * workaround can break.  Also, other PIL 15 interrupts besides the ones we are
 * enveloping could delay processing of the interrupt we are trying to protect.
 */
int pxtool_cfg_delay_usec = 2500;
int pxtool_iomem_delay_usec = 25000;

/* Currently there is no way of getting this info from hypervisor. */
#define	INTERRUPT_MAPPING_ENTRIES	64

/* Number of inos per root complex. */
int pxtool_num_inos = INTERRUPT_MAPPING_ENTRIES;

static int
pxtool_phys_access(px_t *px_p, uintptr_t dev_addr,
    uint64_t *data_p, boolean_t is_big_endian, boolean_t is_write)
{
	void *func;
	uint64_t rfunc;
	uint64_t pfunc;
	uint64_t rdata_addr;
	uint64_t pdata_addr;
	int rval;
	dev_info_t *dip = px_p->px_dip;

	DBG(DBG_TOOLS, dip,
	    "pxtool_phys_access: dev_addr:0x%" PRIx64 "\n", dev_addr);
	DBG(DBG_TOOLS, dip, "    data_addr:0x%" PRIx64 ", is_write:%s\n",
	    data_p, (is_write ? "yes" : "no"));

	/*LINTED*/
	func = (is_write) ? (void *)px_phys_poke_4v : (void *)px_phys_peek_4v;

	if ((rfunc = va_to_pa(func))  == (uint64_t)-1) {
		DBG(DBG_TOOLS, dip, "Error getting real addr for function\n");
		return (EIO);
	}

	if ((pfunc = hv_ra2pa(rfunc)) == -1) {
		DBG(DBG_TOOLS, dip, "Error getting phys addr for function\n");
		return (EIO);
	}

	if ((rdata_addr = va_to_pa((void *)data_p))  == (uint64_t)-1) {
		DBG(DBG_TOOLS, dip, "Error getting real addr for data_p\n");
		return (EIO);
	}

	if ((pdata_addr = hv_ra2pa(rdata_addr)) == -1) {
		DBG(DBG_TOOLS, dip, "Error getting phys addr for data ptr\n");
		return (EIO);
	}

	rval = hv_hpriv((void *)pfunc, dev_addr, pdata_addr, is_big_endian);
	switch (rval) {
	case H_ENOACCESS:	/* Returned by non-debug hypervisor. */
		rval = ENOTSUP;
		break;
	case H_EOK:
		rval = SUCCESS;
		break;
	default:
		rval = EIO;
		break;
	}

	return (rval);
}

/* Swap endianness. */
static uint64_t
pxtool_swap_endian(uint64_t data, int size)
{
	typedef union {
		uint64_t data64;
		uint8_t data8[8];
	} data_split_t;

	data_split_t orig_data;
	data_split_t returned_data;
	int i;

	orig_data.data64 = data;
	returned_data.data64 = 0;

	for (i = 0; i < size; i++) {
		returned_data.data8[7 - i] = orig_data.data8[8 - size + i];
	}

	return (returned_data.data64);
}

/*
 * This function is for PCI config space access.
 * It assumes that offset, bdf, acc_attr are valid in prg_p.
 * It assumes that prg_p->phys_addr is the final phys addr (including offset).
 * This function modifies prg_p status and data.
 */

/*ARGSUSED*/
int
pxtool_pcicfg_access(px_t *px_p, pcitool_reg_t *prg_p,
    uint64_t max_addr, uint64_t *data_p, boolean_t is_write)
{
	pci_cfg_data_t data;
	on_trap_data_t otd;
	dev_info_t *dip = px_p->px_dip;
	px_pec_t *pec_p = px_p->px_pec_p;
	pci_device_t bdf = PX_GET_BDF(prg_p);
	size_t size = PCITOOL_ACC_ATTR_SIZE(prg_p->acc_attr);
	int rval = 0;

	/* Alignment checking. */
	if (!IS_P2ALIGNED(prg_p->offset, size)) {
		DBG(DBG_TOOLS, dip, "not aligned.\n");
		prg_p->status = PCITOOL_NOT_ALIGNED;
		return (EINVAL);
	}

	mutex_enter(&pec_p->pec_pokefault_mutex);
	pec_p->pec_ontrap_data = &otd;

	if (is_write) {

		if (PCITOOL_ACC_IS_BIG_ENDIAN(prg_p->acc_attr))
			data.qw = pxtool_swap_endian(*data_p, size);
		else
			data.qw = *data_p;

		switch (size) {
			case sizeof (uint8_t):
				data.b = (uint8_t)data.qw;
				break;
			case sizeof (uint16_t):
				data.w = (uint16_t)data.qw;
				break;
			case sizeof (uint32_t):
				data.dw = (uint32_t)data.qw;
				break;
			case sizeof (uint64_t):
				break;
		}

		DBG(DBG_TOOLS, dip, "put: bdf:0x%x, off:0x%" PRIx64 ", size:"
		    "0x%" PRIx64 ", data:0x%" PRIx64 "\n",
		    bdf, prg_p->offset, size, data.qw);

		pec_p->pec_safeacc_type = DDI_FM_ERR_POKE;

		if (!on_trap(&otd, OT_DATA_ACCESS)) {
			otd.ot_trampoline = (uintptr_t)&poke_fault;
			rval = hvio_config_put(px_p->px_dev_hdl, bdf,
			    prg_p->offset, size, data);
		} else
			rval = H_EIO;

		if (otd.ot_trap & OT_DATA_ACCESS)
			rval = H_EIO;

	} else {

		data.qw = 0;

		pec_p->pec_safeacc_type = DDI_FM_ERR_PEEK;

		if (!on_trap(&otd, OT_DATA_ACCESS)) {
			otd.ot_trampoline = (uintptr_t)&peek_fault;
			rval = hvio_config_get(px_p->px_dev_hdl, bdf,
			    prg_p->offset, size, &data);
		} else
			rval = H_EIO;

		DBG(DBG_TOOLS, dip, "get: bdf:0x%x, off:0x%" PRIx64 ", size:"
		    "0x%" PRIx64 ", data:0x%" PRIx64 "\n",
		    bdf, prg_p->offset, size, data.qw);

		switch (size) {
			case sizeof (uint8_t):
				*data_p = data.b;
				break;
			case sizeof (uint16_t):
				*data_p = data.w;
				break;
			case sizeof (uint32_t):
				*data_p = data.dw;
				break;
			case sizeof (uint64_t):
				*data_p = data.qw;
				break;
			default:
				DBG(DBG_TOOLS, dip,
				    "bad size:0x%" PRIx64 "\n", size);
				break;
		}
		if (PCITOOL_ACC_IS_BIG_ENDIAN(prg_p->acc_attr))
			*data_p = pxtool_swap_endian(*data_p, size);
	}

	/*
	 * Workaround: delay taking down safe access env.
	 * For more info, see comments where pxtool_cfg_delay_usec is declared.
	 */
	if (pxtool_cfg_delay_usec > 0)
		drv_usecwait(pxtool_cfg_delay_usec);

	no_trap();
	pec_p->pec_ontrap_data = NULL;
	pec_p->pec_safeacc_type = DDI_FM_ERR_UNEXPECTED;
	mutex_exit(&pec_p->pec_pokefault_mutex);

	if (rval != SUCCESS) {
		prg_p->status = PCITOOL_INVALID_ADDRESS;
		rval = EINVAL;
	} else
		prg_p->status = PCITOOL_SUCCESS;

	return (rval);
}


/*
 * This function is for PCI IO space and memory space access.
 * It assumes that offset, bdf, acc_attr are current in prg_p.
 * It assumes that prg_p->phys_addr is the final phys addr (including offset).
 * This function modifies prg_p status and data.
 */
int
pxtool_pciiomem_access(px_t *px_p, pcitool_reg_t *prg_p, uint64_t max_addr,
    uint64_t *data_p, boolean_t is_write)
{
	on_trap_data_t otd;
	uint32_t io_stat = 0;
	dev_info_t *dip = px_p->px_dip;
	px_pec_t *pec_p = px_p->px_pec_p;
	size_t size = PCITOOL_ACC_ATTR_SIZE(prg_p->acc_attr);
	int rval = 0;

	/* Alignment checking. */
	if (!IS_P2ALIGNED(prg_p->offset, size)) {
		DBG(DBG_TOOLS, dip, "not aligned.\n");
		prg_p->status = PCITOOL_NOT_ALIGNED;
		return (EINVAL);
	}

	/* Upper bounds checking. */
	if (prg_p->phys_addr > max_addr) {
		DBG(DBG_TOOLS, dip, "Phys addr 0x%" PRIx64 " out of "
		    "range (max 0x%" PRIx64 ").\n", prg_p->phys_addr, max_addr);
		prg_p->status = PCITOOL_INVALID_ADDRESS;
		return (EINVAL);
	}

	mutex_enter(&pec_p->pec_pokefault_mutex);
	pec_p->pec_ontrap_data = &otd;

	if (is_write) {
		pci_device_t bdf = PX_GET_BDF(prg_p);

		if (PCITOOL_ACC_IS_BIG_ENDIAN(prg_p->acc_attr))
			*data_p = pxtool_swap_endian(*data_p, size);

		pec_p->pec_safeacc_type = DDI_FM_ERR_POKE;

		if (!on_trap(&otd, OT_DATA_ACCESS)) {
			otd.ot_trampoline = (uintptr_t)&poke_fault;
			rval = hvio_poke(px_p->px_dev_hdl, prg_p->phys_addr,
			    size, *data_p, bdf, &io_stat);
		} else
			rval = H_EIO;

		if (otd.ot_trap & OT_DATA_ACCESS)
			rval = H_EIO;

		DBG(DBG_TOOLS, dip, "iomem:phys_addr:0x%" PRIx64 ", bdf:0x%x, "
		    "rval:%d, io_stat:%d\n", prg_p->phys_addr, bdf,
		    rval, io_stat);
	} else {

		*data_p = 0;

		pec_p->pec_safeacc_type = DDI_FM_ERR_PEEK;

		if (!on_trap(&otd, OT_DATA_ACCESS)) {
			otd.ot_trampoline = (uintptr_t)&peek_fault;
			rval = hvio_peek(px_p->px_dev_hdl, prg_p->phys_addr,
			    size, &io_stat, data_p);
		} else
			rval = H_EIO;

		DBG(DBG_TOOLS, dip, "iomem:phys_addr:0x%" PRIx64 ", "
		    "size:0x%" PRIx64 ", hdl:0x%" PRIx64 ", "
		    "rval:%d, io_stat:%d\n", prg_p->phys_addr,
		    size, px_p->px_dev_hdl, rval, io_stat);
		DBG(DBG_TOOLS, dip, "read data:0x%" PRIx64 "\n", *data_p);

		if (PCITOOL_ACC_IS_BIG_ENDIAN(prg_p->acc_attr))
			*data_p = pxtool_swap_endian(*data_p, size);
	}

	/*
	 * Workaround: delay taking down safe access env.
	 * For more info, see comment where pxtool_iomem_delay_usec is declared.
	 */
	if (pxtool_iomem_delay_usec > 0)
		delay(drv_usectohz(pxtool_iomem_delay_usec));

	no_trap();
	pec_p->pec_ontrap_data = NULL;
	pec_p->pec_safeacc_type = DDI_FM_ERR_UNEXPECTED;
	mutex_exit(&pec_p->pec_pokefault_mutex);

	if (rval != SUCCESS) {
		prg_p->status = PCITOOL_INVALID_ADDRESS;
		rval = EINVAL;
	} else if (io_stat != SUCCESS) {
		prg_p->status = PCITOOL_IO_ERROR;
		rval = EIO;
	} else
		prg_p->status = PCITOOL_SUCCESS;

	return (rval);
}


/*ARGSUSED*/
int
pxtool_dev_reg_ops_platchk(dev_info_t *dip, pcitool_reg_t *prg_p)
{
	return (SUCCESS);
}


/*
 * Perform register accesses on the nexus device itself.
 */
int
pxtool_bus_reg_ops(dev_info_t *dip, void *arg, int cmd, int mode)
{

	pcitool_reg_t		prg;
	size_t			size;
	px_t			*px_p = DIP_TO_STATE(dip);
	boolean_t		is_write = B_FALSE;
	uint32_t		rval = 0;

	if (cmd == PCITOOL_NEXUS_SET_REG)
		is_write = B_TRUE;

	DBG(DBG_TOOLS, dip, "pxtool_bus_reg_ops set/get reg\n");

	/* Read data from userland. */
	if (ddi_copyin(arg, &prg, sizeof (pcitool_reg_t),
	    mode) != DDI_SUCCESS) {
		DBG(DBG_TOOLS, dip, "Error reading arguments\n");
		return (EFAULT);
	}

	size = PCITOOL_ACC_ATTR_SIZE(prg.acc_attr);

	DBG(DBG_TOOLS, dip, "raw bus:0x%x, dev:0x%x, func:0x%x\n",
	    prg.bus_no, prg.dev_no, prg.func_no);
	DBG(DBG_TOOLS, dip, "barnum:0x%x, offset:0x%" PRIx64 ", acc:0x%x\n",
	    prg.barnum, prg.offset, prg.acc_attr);
	DBG(DBG_TOOLS, dip, "data:0x%" PRIx64 ", phys_addr:0x%" PRIx64 "\n",
	    prg.data, prg.phys_addr);

	/*
	 * If bank num == ff, base phys addr passed in from userland.
	 *
	 * Normal bank specification is invalid, as there is no OBP property to
	 * back it up.
	 */
	if (prg.barnum != PCITOOL_BASE) {
		prg.status = PCITOOL_OUT_OF_RANGE;
		rval = EINVAL;
		goto done;
	}

	/* Allow only size of 8-bytes. */
	if (size != sizeof (uint64_t)) {
		prg.status = PCITOOL_INVALID_SIZE;
		rval = EINVAL;
		goto done;
	}

	/* Alignment checking. */
	if (!IS_P2ALIGNED(prg.offset, size)) {
		DBG(DBG_TOOLS, dip, "not aligned.\n");
		prg.status = PCITOOL_NOT_ALIGNED;
		rval = EINVAL;
		goto done;
	}

	prg.phys_addr += prg.offset;

	/*  XXX do some kind of checking here? */

	/* Access device.  prg.status is modified. */
	rval = pxtool_phys_access(px_p, prg.phys_addr, &prg.data,
	    PCITOOL_ACC_IS_BIG_ENDIAN(prg.acc_attr), is_write);
done:
	if (ddi_copyout(&prg, arg, sizeof (pcitool_reg_t),
	    mode) != DDI_SUCCESS) {
		DBG(DBG_TOOLS, dip, "Copyout failed.\n");
		return (EFAULT);
	}

	return (rval);
}
