/**
 * @filename nandflash.c
 * @brief ak980x nandflash driver
 * Copyright (C) 2006 Anyka (Guangzhou) Software Technology Co., LTD
 * @author yiruoxiang
 * @modify jiangdihui
 * @date 2007-1-10
 * @version 1.0
 * @ref Please refer to…
 */
#include <linux/kernel.h>
#include <asm/delay.h>
#include <mach-anyka/anyka_types.h>
#include <asm/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <mach/l2.h>
#include <mach/gpio.h>
#include "anyka_cpu.h"
#include "arch_nand.h"
#include "nand_control.h"
#include "sysctl.h"

#define READ_BUF	GLOBE_BUF4
#define WRITE_BUF	GLOBE_BUF5

#define NAND_L2_ENABLE	1	/* Define NAND_L2_ENABLE to use new L2 API(ak98_l2_*), undefine it to use old communicate.c API */
//#undef NAND_L2_ENABLE

#define ZZ_DEBUG        0

#ifdef CONFIG_MTD_NAND_TEST
extern T_U32 *nand_erase_test;
extern T_U32 nand_total_blknum;
#endif

extern dma_addr_t dmahandle;
extern void *bufaddr;


//*****************************************************************************************
static T_BOOL   nf_check_data_spare_separate(T_PNAND_ECC_STRU pDataCtrl, T_PNAND_ECC_STRU pSpareCtrl, T_U32 *cycle, T_U32 *effective_size, T_U32 *ecc_type);
static T_U32    nf_get_ecccode_len(T_U32 ecc_type);
static T_U8     nf_read_chipstatus(T_U32 Chip);
static T_U32*   nf_send_addr(T_U32 *reg_addr, T_U32 columnAddr,T_U32 rowAddr, T_U32 col_cycle, T_U32 row_cycle);
//static T_VOID   nf_controller_reset(T_VOID);
static T_BOOL   check_cmd_done(T_VOID);
static T_VOID   cmd_go(T_U32 Chip);
static T_U8     nf_check_ecc_status(T_U32 stat);
static T_U32    calc_new(T_U8 data_opt_len, T_U8 data_we_fe, T_U8 data_we_re, T_U8 data_re_fe, T_U8 data_re_re);
static T_U32    ecc_repair(T_U32 wrong_info, T_U8 *main_buf, T_U8 *add_buf, T_U32 main_size, T_U32 add_size, T_U32 ecc_type);
static T_U32    nf_check_repair_ecc(T_U8 *pMain_buf, T_U8 *pAdd_buf, T_U32 section_size, T_U32 add_len, T_U32 ecc_type, T_BOOL  bSeparate_spare);

//*****************************************************************************************
typedef enum {
	USB_BULK_SEND,
	USB_BULK_RECE,
	USB_ISO,
	NAND_FLASH,
	MMC_SD1,
	MMC_SD2,
	MMC_SD3,
	SPI1_RECE,
	SPI1_SEND,
	DAC,
	SPI2_RECE,
	SPI2_SEND,
	GPS
} PERIPHERAL_TYPE;

typedef enum {
	GLOBE_BUF0 = 0,
	GLOBE_BUF1,
	GLOBE_BUF2,
	GLOBE_BUF3,
	GLOBE_BUF4,
	GLOBE_BUF5,
	GLOBE_BUF6,
	GLOBE_BUF7
} GLOBE_BUF_ID;

extern struct semaphore  nand_lock;

extern T_U8 communicate_conf(PERIPHERAL_TYPE dev_type, GLOBE_BUF_ID buf_id);
extern T_U8 rece_dat_cpu(T_U8 * buf, T_U32 len, PERIPHERAL_TYPE dev_type);
extern T_U8 prepare_dat_send_cpu(const T_U8 * buf, T_U32 len, PERIPHERAL_TYPE dev_type);

//***************************************************************************************    
#define MAX_LOOP_CNT       T_U32_MAX

#define CAL_40M(n)   ((((n) - 1) >> 1) + 1)     //即使n＝0，也会算出-1+1=0
#define CAL_62M(n)   (((((n) * 3) - 1 ) >> 2) + 1)  // a >= (3*a_84 - 1)/4
#define CAL_124M(n)  ((((n) * 3) >> 1) + 1)
#define CAL_168M(n)  (((n) * 2) + 1)

#define DEF_FRQ                     84
#define CAL_TIM_LOW(freq, val)      (((freq) * (val) + ((DEF_FRQ) - 1)) / (DEF_FRQ))
#define CAL_TIM_HIGH(freq, val)     (((freq) * ((val) + 1) + ((DEF_FRQ) - 1)) / (DEF_FRQ) - 1)


/** @name frequency define
         define frequency by numbers
 */
/*@{*/
#define FREQ_168M       (168 * 1000000)
#define FREQ_124M       (124 * 1000000)
#define FREQ_84M        (84 * 1000000)
#define FREQ_62M        (62 * 1000000)
#define FREQ_31M        (31 * 1000000)
/*@} */

static T_U32 s_datalen_40M  = 0x82627;
static T_U32 s_datalen_62M  = 0x82627;
static T_U32 s_cmdlen_84M   = 0x82671;
static T_U32 s_datalen_84M  = 0x82627;
static T_U32 s_datalen_124M = 0xf5c5c;
static T_U32 s_datalen_168M = 0xf5c5c;

static T_U8 s_flip_bits = 0;

#define ONLY_WEAK_DANGER_PRINT  //only when ecc flip is too much enough to alert user, print error info

#ifdef ONLY_WEAK_DANGER_PRINT
#define MAX_ECC_FLIP_BIT_NUM 32

#define DANGER_BIT_NUM_MODE0 2	  //	4 bit nand ecc's danger flip bit number among 512 Bytes
#define DANGER_BIT_NUM_MODE1 6  //8 bit nand ecc's danger flip bit number among 512 Bytes
#define DANGER_BIT_NUM_MODE2 8  //12 bit nand ecc's danger flip bit number among 512 Bytes
#define DANGER_BIT_NUM_MODE3 12  //16 bit nand ecc's danger flip bit number among 512 Bytes
#define DANGER_BIT_NUM_MODE4 18  //24 bit nand ecc's danger flip bit number among 1024 Bytes
#define DANGER_BIT_NUM_MODE5 26  //32 bit nand ecc's danger flip bit number among 1024 Bytes

typedef struct
{
    T_U16 err_loc[MAX_ECC_FLIP_BIT_NUM];
    T_U8 bad_bytes[MAX_ECC_FLIP_BIT_NUM];
    T_U8 good_bytes[MAX_ECC_FLIP_BIT_NUM];
}T_ECC_INFO;

static T_ECC_INFO s_ecc_info;
#endif

/**
 * @brief initialization of nandflash hardware.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @return  T_VOID
 */ 

T_VOID nand_HWinit(T_VOID)
{

    //T_U8 chip;
    
    down(&nand_lock);
    
    //set share pin as nandflash data pin
    ak98_group_config(ePIN_AS_NFC);

    //set cmd&data cycle\len
    REG32(FLASH_CTRL_REG23) = 0xf5ad1;//0x41230//0x52341//0x82671//0xC3671
    REG32(FLASH_CTRL_REG24) = 0xf5c5c;//0x30102//0x41213//0x82627//0xF3637

    #if 0 
    for (chip = 0; chip < 4; chip++)
    {
        nand_reset(chip);
    }
    #endif
    nand_reset(0);
    up(&nand_lock);   
	
}

/**
 * @brief config nand command and data cycle
 *
 * @author xuchang
 * @date 2007-12-27
 * @param[in] CmdCycle the command cycle to config
 * @param[in] DataCycle the data cycle to config
 * @return T_VOID
 */
T_VOID nand_config_timeseq(T_U32 CmdCycle, T_U32 DataCycle)
{
    //open nand controller clock so that register can be read!!
    //DRV_PROTECT
    //set cmd&data cycle\len
    REG32(FLASH_CTRL_REG23) = CmdCycle;
    REG32(FLASH_CTRL_REG24) = DataCycle;
    //DRV_UNPROTECT
}

static T_U32 calctiming(T_U32 freq)
{
    T_U32 data_len;
    T_U8 data_opt_len, data_we_fe, data_we_re, data_re_fe, data_re_re;
    T_U8 data_opt_len_new, data_we_fe_new, data_we_re_new, data_re_fe_new, data_re_re_new;
    T_U32 freq_tmp = freq / 1000000;

    if (s_datalen_84M > 0x90000)
    {
        data_opt_len = 8;
        data_we_fe = 2;
        data_we_re = 6;
        data_re_fe = 2;
        data_re_re = 7;
    }
    else
    {
        data_opt_len = (s_datalen_84M >> 16) & 0xF;
        data_we_fe = (s_datalen_84M >> 12) & 0xF;
        data_we_re = (s_datalen_84M >> 8) & 0xF;
        data_re_fe = (s_datalen_84M >> 4) & 0xF;
        data_re_re = (s_datalen_84M >> 0) & 0xF;
    }

    if (freq <= FREQ_84M)
    {
        data_opt_len_new = CAL_TIM_LOW(freq_tmp, data_opt_len);
        data_we_fe_new   = CAL_TIM_LOW(freq_tmp, data_we_fe);
        data_we_re_new   = CAL_TIM_LOW(freq_tmp, data_we_re);
        data_re_fe_new   = CAL_TIM_LOW(freq_tmp, data_re_fe);
        data_re_re_new   = CAL_TIM_LOW(freq_tmp, data_re_re);
    }
    else
    {
        data_opt_len_new = CAL_TIM_HIGH(freq_tmp, data_opt_len);
        data_we_fe_new   = CAL_TIM_HIGH(freq_tmp, data_we_fe);
        data_we_re_new   = CAL_TIM_HIGH(freq_tmp, data_we_re);
        data_re_fe_new   = CAL_TIM_HIGH(freq_tmp, data_re_fe);
        data_re_re_new   = CAL_TIM_HIGH(freq_tmp, data_re_re);
    }    

    
    data_len = calc_new(data_opt_len_new, data_we_fe_new, data_we_re_new, data_re_fe_new, data_re_re_new);

    return data_len;
}    

/**
 * @brief calculate each nand's timing under 62MHz & 124MHz
 *
 * @author yiruoxiang
 * @date 2007-12-27
 * @param[in] DefDataLen default data lenght
 * @return  T_VOID
 */
T_VOID nand_calctiming(T_U32 DefDataLen)
{
     //open nand controller clock so that register can be read!!
    //DRV_PROTECT
    //record the original data_len under 84MHz
    s_cmdlen_84M = REG32(FLASH_CTRL_REG23);
    s_datalen_84M = DefDataLen;
    //DRV_UNPROTECT
}


/**
 * @brief change nand timing when Freq has changed
 *
 * @author yiruoxiang
 * @date 2007-12-27
 * @param[in] Freq frequency
 * @return  T_VOID
 */
T_VOID nand_changetiming(T_U32 Freq)
{
    T_U32 cmd_len, dat_len;

    if (Freq > FREQ_84M)
    {
        cmd_len = 0xf5ad1;
    }
    else 
    {
        cmd_len = 0x82671;
    }

    dat_len = calctiming(Freq);

    nand_config_timeseq(cmd_len, dat_len);
}

/**
 * @brief read nand flash chip ID.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be read.
 * @return  T_U32
 * @retval  current nandflash ID
 */
T_U32 nand_read_chipID(T_U32 Chip)
{
    T_U32 nand_id = 0;

    down(&nand_lock);
    ak98_group_config(ePIN_AS_NFC);
    
    //clear internal status     
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;
    
    //config command
    REG32(FLASH_CTRL_REG0 + 0x00) = (NFLASH_READ_ID << 11) | COMMAND_CYCLES_CONF;

    //config address
    REG32(FLASH_CTRL_REG0 + 0x04) = (0x00 << 11) | ADDRESS_CYCLES_CONF;

    //ID information is 4 byte,read it to register 22
    REG32(FLASH_CTRL_REG0 + 0x08) = (3 << 11) | READ_INFO_CONF | LAST_CMD_FLAG;

    //excute operation
    cmd_go(Chip);

    // wait end & read data (data at where)
    while ( !check_cmd_done() );

    // read status
    nand_id = REG32(FLASH_CTRL_REG20);
    up(&nand_lock);
    return nand_id;
}

/**
 * @brief reset nand flash.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be reset.
 * @return  T_VOID
 */
T_VOID nand_reset(T_U32 Chip)
{    
    //clear internal status
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    //config command
    REG32(FLASH_CTRL_REG0 + 0x00) = (NFLASH_RESET << 11) | COMMAND_CYCLES_CONF;

    //config wait time, the chip may not exist
    REG32(FLASH_CTRL_REG0 + 0x04) = ((84 * 1000 / 1024) << 11) | DELAY_CNT_CONF | LAST_CMD_FLAG; // wait R/B rising

    //excute operation
    cmd_go(Chip);

    // wait end & read data (data at where)
    while ( !check_cmd_done() );   // 等待操作完成
   
}

/* set timing */ 
void nand_settiming(unsigned long cmd_timing, unsigned long data_timing)
{
    //printk(KERN_INFO "zz ak98-nfc.c ak98_nand_settiming() line 335\n");
    down(&nand_lock);
    ak98_group_config(ePIN_AS_NFC);
	if (cmd_timing)
		*(volatile unsigned long *)(FLASH_CTRL_REG23) = cmd_timing;

	if (data_timing)
		*(volatile unsigned long *)(FLASH_CTRL_REG24) = data_timing;
		
	up(&nand_lock);
}

/* get nandflash chip status */
unsigned char nand_get_status(unsigned char chip)
{
	unsigned long nand_status;
	unsigned long *reg_addr;
    //printk(KERN_INFO "zz ak98-nfc.c ak98_nand_get_status() line 349\n");
    down(&nand_lock);
    ak98_group_config(ePIN_AS_NFC);

	do {
		nand_status = 0;
		reg_addr = (unsigned long *)FLASH_CTRL_REG0;

		*(volatile unsigned long *)(FLASH_CTRL_REG22) = 0x00;

		/* send cmd */
		*(volatile unsigned long *)(reg_addr++) =
		    ((NFLASH_READ_STATUS << 11) | COMMAND_CYCLES_CONF);
		*(volatile unsigned long *)(reg_addr++) = DELAY_CNT_CONF;
		*(volatile unsigned long *)(reg_addr++) = DELAY_CNT_CONF;
		*(volatile unsigned long *)(reg_addr) =
		    ((0x1 << 11) | READ_INFO_CONF | LAST_CMD_FLAG);

		/* excute operation, , enable power saving, CE# keep LOW wait R/B */
		*(volatile unsigned long *)(FLASH_CTRL_REG22) =
		    NCHIP_SELECT(chip) | DEFAULT_GO;

		while (!check_cmd_done()) ;

		/* read status */
		nand_status = *(volatile unsigned long *)(FLASH_CTRL_REG20);
		if ((nand_status & AK_NAND_STATUS_READY) !=
		    AK_NAND_STATUS_READY)
			mdelay(2);
		else
			break;
	} while (1);

	if ((nand_status & AK_NAND_STATUS_ERROR) == AK_NAND_STATUS_ERROR)
		printk("status error!\n");

    up(&nand_lock);
	return nand_status & 0xFF;
}


/**
 * @brief read data from nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[in/out] pDataCtrl control reading data section: buffer, data lenght, ECC.
 * @param[in/out] pSpareCtrl control reading spare section: buffer, data lenght, ECC.
 * @return  T_U32
 * @retval  0 success, 1 fail
 */
T_U32 nand_readpage_ecc(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_PNAND_ECC_STRU  pDataCtrl,  T_PNAND_ECC_STRU pSpareCtrl)
{
    T_U32   i, regvalue, loopCnt, ret;
    T_U32   ecc_type;
    T_U32   effective_size, total_len, section_size =0, add_len;
    T_U8    *pMain_buf, *pAdd_buf;
    T_U32   *reg_addr;
    T_U16   fail_cnt = 0;
    T_BOOL  bSeparate_spare, bSpare_read;
    T_U8    l2_buf_id;    
    T_U8    danger_bit_num;     //if ecc flip bit number is more than danger_bit_num, print all flip bytes info
    T_BOOL  bIsFinalRead = AK_FALSE;     //final read, not care previous read_fail
   
    down(&nand_lock);
    //-------------------zhangzheng add start-----------------------------
	#ifndef NAND_L2_ENABLE
    asm("AK_InvalidateDCache_z:\n" "mrc  p15,0,r15,c7,c14,3\n" "bne AK_InvalidateDCache_z"); 
	#endif
    ak98_group_config(ePIN_AS_NFC);
        
    //-------------------zhangzheng add end-----------------------------
RETRY_RP:
    ret = 0;
    bSpare_read = AK_FALSE;
    pMain_buf = AK_NULL;
    pAdd_buf = AK_NULL;
    add_len = 0;
    l2_buf_id = BUF_NULL;

    /*check whether is data and spare separate, get the length and ECC type of the first data seciton or spare,
           and get the times to read data*/ 
    bSeparate_spare = nf_check_data_spare_separate(pDataCtrl, pSpareCtrl, &loopCnt, &effective_size, &ecc_type);
	#if ZZ_DEBUG
    printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 398 Chip=%d,RowAddr=%d,ColumnAddr=%d\n",Chip, RowAddr,ColumnAddr);
    printk(KERN_INFO "bSeparate_spare=%d loopCnt=%d effective_size=%d ecc_type=%d\n",
    bSeparate_spare,loopCnt,effective_size,ecc_type);
	#endif

    if(0 == loopCnt)
    {
    	printk(KERN_INFO "%s:%d: loopCnt=%lu \n", __func__, __LINE__, loopCnt);
        ret = 1;
        goto RETURN2;
    }   

    
#ifdef ONLY_WEAK_DANGER_PRINT
    if (ECC_4BIT_P512B == pNF_Add->EccType)
    {
        danger_bit_num = DANGER_BIT_NUM_MODE0;
    }
    else if (ECC_8BIT_P512B == pNF_Add->EccType)
    {
        danger_bit_num = DANGER_BIT_NUM_MODE1;
    }
    else if (ECC_12BIT_P512B == pNF_Add->EccType)
    {
        danger_bit_num = DANGER_BIT_NUM_MODE2;
    }
    else if (ECC_24BIT_P1KB == pNF_Add->EccType)
    {
        danger_bit_num = DANGER_BIT_NUM_MODE4;
    }
    else //default (ECC_32BIT_P1KB == pNF_Add->EccType)
    {
        danger_bit_num = DANGER_BIT_NUM_MODE5;
    }
#endif

    //clear status
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    //nandflash read1 command
    reg_addr = (T_U32 *)FLASH_CTRL_REG0;      
    if (NAND_512B_PAGE != pNF_Add->ChipType)
    {
        REG32(reg_addr++) = (NFLASH_READ1 << 11) | COMMAND_CYCLES_CONF;
    }
    else
    {
        if(ColumnAddr < 256)
        {
            REG32(reg_addr++) = (NFLASH_READ1 << 11) | COMMAND_CYCLES_CONF;
        }
        else if(ColumnAddr >= 512)
        {
            REG32(reg_addr++) = (NFLASH_READ22 << 11) | COMMAND_CYCLES_CONF;
        }
        else
        {
            REG32(reg_addr++) = (NFLASH_READ1_HALF << 11) | COMMAND_CYCLES_CONF;
        }
    }

    //nandflash send address   
    reg_addr = nf_send_addr(reg_addr, 0, RowAddr, pNF_Add->ColCycle, pNF_Add->RowCycle);

    //nandflash read2 command if nand is large page
    if (NAND_512B_PAGE != pNF_Add->ChipType)
    {
        REG32(reg_addr++) = (NFLASH_READ2 << 11) | COMMAND_CYCLES_CONF;
    }
    //config waiting R/B rising edge
    REG32(reg_addr++) = WAIT_JUMP_CONF | LAST_CMD_FLAG;

    //run command, wait cmd done 
    cmd_go(Chip);
	#if ZZ_DEBUG
	printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 447 before while(cmd)\n");
	#endif
	
    while (!check_cmd_done());
	
	#if ZZ_DEBUG
	printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 453 before while(cmd)\n");
	#endif
	
#ifdef NAND_L2_ENABLE
    l2_buf_id = ak98_l2_alloc(ADDR_NFC);
    if (BUF_NULL == l2_buf_id)
    {
    	printk(KERN_INFO "%s:%d:I2_buf_id=BUF_NULL \n", __func__, __LINE__);
        ret = 1;
        goto RETURN2;
    }    
#else
    communicate_conf(NAND_FLASH, READ_BUF);
#endif
    for (i = 0; i < loopCnt; i++)
    {
        T_U32 ecc_ret;
        /*set data size and ecc type to read when reading spare section 
              if data and spare is separate */
        if(bSeparate_spare  && (( loopCnt - 1) == i) && (AK_NULL != pSpareCtrl))
        {
            bSpare_read = AK_TRUE;
            effective_size = pSpareCtrl->buf_len;
            ecc_type = pSpareCtrl->ecc_type;     
        }

        //config ECC module
        regvalue = ECC_CTL_DEC_EN | ECC_CTL_DIR_READ | ECC_CTL_ADDR_CLR      \
                        | ECC_CTL_BYTE_CFG(effective_size) | ECC_CTL_NFC_EN | ECC_CTL_MODE(ecc_type)    \
                        | ECC_CTL_NO_ERR | ECC_CTL_RESULT_NO_OK;    // reset ecc result status bits
        HAL_WRITE_UINT32(FLASH_ECC_REG0, regvalue);
        HAL_WRITE_UINT32(FLASH_ECC_REG0,  regvalue|ECC_CTL_START);

        //clear status
        REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

        //config the whole length of a section including ecc part
        total_len = effective_size + nf_get_ecccode_len(ecc_type);
		#if ZZ_DEBUG
        printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 491 total_len=%d\n",
               total_len);
		#endif
        REG32(FLASH_CTRL_REG0) = ((total_len - 1) << 11) | READ_DATA_CONF | LAST_CMD_FLAG;
#ifdef NAND_L2_ENABLE
        //clear l2 status, must before cmd_go
        ak98_l2_clr_status(l2_buf_id);
#endif
        
        //start to transfer data from nand to l2
        cmd_go(Chip);

        //need to read data part if pDataCtrl is not NULL
        if ((AK_NULL != pDataCtrl) && (!bSpare_read))
        {            
            if(!bSeparate_spare)
            {
                section_size = pDataCtrl->ecc_section_len - pSpareCtrl->buf_len;
            }
            else
            {
                section_size = pDataCtrl->ecc_section_len;
            }    
			#if ZZ_DEBUG
			printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 515 section_size=%d\n", section_size);
			#endif
            pMain_buf = pDataCtrl->buf + (i * section_size);
#ifdef NAND_L2_ENABLE
            
			ak98_l2_combuf_cpu((T_U32)pMain_buf, l2_buf_id, section_size, BUF2MEM); 
#else	// ! NAND_L2_ENABLE
        	rece_dat_cpu(pMain_buf, section_size, NAND_FLASH);
#endif
        } 
//        printk(KERN_INFO "zz nand_control.c nand_readpage_ecc()592 reg22=0x%x \n",*(volatile unsigned long *)(FLASH_CTRL_REG22));
        //wait data transfered        
        while(!check_cmd_done());
//        printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 595\n");
        //copy spare from l2 to memory 
        if ((AK_NULL != pSpareCtrl) && (!bSeparate_spare || bSpare_read))
        {  
            if(!bSeparate_spare)
            {
                //need to read spare while reading a section of data if data and spare is not separate
                pAdd_buf = pSpareCtrl->buf;
                add_len = pSpareCtrl->buf_len;
            }  
            else
            {
                //read spare for the last time(bSpare_read is true) if data and spare is separate
                pMain_buf = pSpareCtrl->buf;
                section_size = effective_size;
            }
			#if ZZ_DEBUG
			printk(KERN_INFO "zz nand_control.c nand_readpage_ecc() 557 spare len=%d\n",
				   pSpareCtrl->buf_len);
			#endif
			
			#ifdef NAND_L2_ENABLE
			ak98_l2_combuf_cpu((T_U32)pSpareCtrl->buf, l2_buf_id, pSpareCtrl->buf_len, BUF2MEM);
			
			#else
			rece_dat_cpu(pSpareCtrl->buf, pSpareCtrl->buf_len, NAND_FLASH);
			#endif

        }

        ecc_ret = nf_check_repair_ecc(pMain_buf, pAdd_buf, section_size, add_len, ecc_type, bSeparate_spare);
      
        if (DATA_ECC_CHECK_ERROR == ecc_ret)
        {   
            if (AK_FALSE == bIsFinalRead)
            {
                fail_cnt++;                
                if ((fail_cnt & 0x40) != 0) //max retry read cnt = 64
                {
                    bIsFinalRead = AK_TRUE;
                }
#ifdef NAND_L2_ENABLE
                ak98_l2_free(ADDR_NFC);
#endif
                goto RETRY_RP;
            }
            else
            {
                ret = 1;
            }
        }
        else if (DATA_ECC_ERROR_REPAIR_CAN_TRUST == ecc_ret)
        {
        
#ifdef ONLY_WEAK_DANGER_PRINT
            if (s_flip_bits > danger_bit_num)
            {
                T_U8 k;

                for (k = 0; k < s_flip_bits; k++)
                {
                    printk(KERN_INFO" L%d", s_ecc_info.err_loc[k]);
                    if (s_ecc_info.good_bytes[k] != s_ecc_info.bad_bytes[k])
                    {
                        printk(KERN_INFO "_0x%x->0x%x", s_ecc_info.bad_bytes[k], s_ecc_info.good_bytes[k]);
                    }
                }
                printk(KERN_INFO "ECC:good,B=%d,P=%d,i=%lu,flip=%u\n", (int)(RowAddr / pNF_Add->PagesPerBlock), (int)(RowAddr % pNF_Add->PagesPerBlock), i, s_flip_bits);
            }
#else
            printk(KERN_INFO "ECC:good,B=%d,P=%d,i=%d,flip=%u\n", (int)(RowAddr / pNF_Add->PagesPerBlock), (int)(RowAddr % pNF_Add->PagesPerBlock), i, s_flip_bits);
#endif
        } 
    }
	
RETURN2:    
#ifdef NAND_L2_ENABLE
    ak98_l2_free(ADDR_NFC);
#endif

    up(&nand_lock);
    return ret;
}   

#if 0
/**
 * @brief read one page(page size>=2048) of data from nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[out] Data buffer for read data, should be large than or equal to 2048 bytes.
 * @param[out] Spare buffer for file system info, should be 4 bytes.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_readsector_large(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 Data[], T_U32 *Spare)
{
    return nand_readsector(Chip, RowAddr, ColumnAddr, pNF_Add, Data, Spare);
}

/**
 * @brief read one page(page size=512) of data from nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[out] Data buffer for read data, should be 512 bytes.
 * @param[out] Spare buffer for file system info, should be 4 bytes.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_readsector(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 Data[], T_U8 *Spare)
{
    T_NAND_ECC_STRU data_ctrl;
    T_NAND_ECC_STRU spare_ctrl;    
    T_U32 ecc_len = (pNF_Add->EccType > ECC_12BIT_P512B) ? NAND_DATA_SIZE_P1KB : NAND_DATA_SIZE_P512B;
	
    data_ctrl.buf = Data;
    data_ctrl.buf_len = pNF_Add->PageSize;    
    data_ctrl.ecc_section_len = ecc_len + NAND_FS_SIZE;    
    data_ctrl.ecc_type = pNF_Add->EccType;

    spare_ctrl.buf = Spare + 2;
    spare_ctrl.buf_len = NAND_FS_SIZE;    
    spare_ctrl.ecc_section_len = ecc_len + NAND_FS_SIZE;    
    spare_ctrl.ecc_type = pNF_Add->EccType;
     
    return nand_readpage_ecc(Chip, RowAddr, ColumnAddr, pNF_Add, &data_ctrl, &spare_ctrl);
}

#endif
/**
 * @brief read file system info.
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[out] Spare buffer for file system info, should be 4 bytes.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_readspare(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 *Spare)
{
    T_NAND_ECC_STRU data_ctrl;
    T_NAND_ECC_STRU spare_ctrl;    
    T_U8 data[NAND_DATA_SIZE_P1KB];
    T_U32 ecc_len = (pNF_Add->EccType > ECC_12BIT_P512B) ? NAND_DATA_SIZE_P1KB : NAND_DATA_SIZE_P512B;

    data_ctrl.buf = data;
    data_ctrl.buf_len = ecc_len;
    data_ctrl.ecc_section_len = ecc_len + NAND_FS_SIZE;
    data_ctrl.ecc_type = pNF_Add->EccType;

    spare_ctrl.buf = (T_U8*)Spare;
    spare_ctrl.buf_len = NAND_FS_SIZE;
    spare_ctrl.ecc_section_len = ecc_len + NAND_FS_SIZE;
    spare_ctrl.ecc_type = pNF_Add->EccType;
    
	#if ZZ_DEBUG
    printk(KERN_INFO "zz nand_control.c nand_readspare() 683\n");
	#endif
    return nand_readpage_ecc(Chip, RowAddr, ColumnAddr, pNF_Add, &data_ctrl, &spare_ctrl);
}

/**
 * @brief read data from nandflash without ECC.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[out] Data buffer for read data.
 * @param[in] Len how many bytes read from nandflash
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_readbytes(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 Data[], T_U32 Len)
{
    T_U32 *reg_addr = (T_U32 *)FLASH_CTRL_REG0;
    T_U32 tmp   = 0;
	T_U8 l2_buf_id = BUF_NULL;
    T_U8  i     = 0;
    T_U32 ret   = 0;
	
	//-------------------zhangzheng add start-----------------------------
	#if ZZ_DEBUG
    printk(KERN_INFO "zz nand_control.c nand_readbytes() 716 Len=%d \n", Len);
	#endif
	#ifndef NAND_L2_ENABLE	
    asm("AK_InvalidateDCache_rb:\n" "mrc  p15,0,r15,c7,c14,3\n" "bne AK_InvalidateDCache_rb"); 
	#endif
    printk(KERN_INFO "nand_readbytes Chip=%lu,RowAddr=%lu,ColumnAddr=%lu \n",Chip, RowAddr,ColumnAddr);

	down(&nand_lock);
    ak98_group_config(ePIN_AS_NFC);
	//-------------------zhangzheng add end------------------------------

    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    //nandflash send command
    REG32(reg_addr++) = (NFLASH_READ1 << 11) | COMMAND_CYCLES_CONF;

    reg_addr = nf_send_addr(reg_addr, ColumnAddr, RowAddr, pNF_Add->ColCycle, pNF_Add->RowCycle);

    //nandflash read2 command,this is a wait command
    if (NAND_512B_PAGE != pNF_Add->ChipType)
    {
        REG32(reg_addr++) = (NFLASH_READ2 << 11) | COMMAND_CYCLES_CONF |(1 << 10);
    }
	
    REG32(reg_addr) = WAIT_JUMP_CONF | LAST_CMD_FLAG;// wait R/B rising edge

	cmd_go(Chip);
	while (!check_cmd_done());

	#ifdef NAND_L2_ENABLE
	l2_buf_id = ak98_l2_alloc(ADDR_NFC);
	if (BUF_NULL == l2_buf_id)
	{
		ret = 1;
		goto RETURN1;
	}
	#else
	communicate_conf(NAND_FLASH, READ_BUF);
	#endif
	
    //config ECC
    tmp = (ECC_CTL_DIR_READ | ECC_CTL_ADDR_CLR  \
            | ECC_CTL_BYTE_CFG(Len) | ECC_CTL_NFC_EN | ECC_CTL_MODE(pNF_Add->EccType));
    HAL_WRITE_UINT32(FLASH_ECC_REG0, tmp);
    HAL_WRITE_UINT32(FLASH_ECC_REG0, tmp|ECC_CTL_START);
    REG32(FLASH_CTRL_REG0) = ((Len - 1) << 11) | READ_DATA_CONF | LAST_CMD_FLAG;
    cmd_go(Chip);

    for(i = 0; i < (Len / NAND_DATA_SIZE); i++)
    {
		#ifdef NAND_L2_ENABLE
		ak98_l2_combuf_cpu((T_U32)(Data + (i * NAND_DATA_SIZE)), l2_buf_id, 
															NAND_DATA_SIZE, BUF2MEM);
		#else
		rece_dat_cpu(Data+(i*NAND_DATA_SIZE), NAND_DATA_SIZE, NAND_FLASH);
		#endif   
    }
	
    tmp = Len & 511;
    if (tmp != 0)
    {
    	#ifdef NAND_L2_ENABLE
		ak98_l2_combuf_cpu((T_U32)(Data + i * NAND_DATA_SIZE), l2_buf_id, tmp, BUF2MEM);
		#else
		rece_dat_cpu(Data+(i*NAND_DATA_SIZE), tmp, NAND_FLASH);
		#endif
    }

    while (!check_cmd_done());

	ret = 0;
	
RETURN1:
	#ifdef NAND_L2_ENABLE 
	ak98_l2_free(ADDR_NFC);
	#endif

	up(&nand_lock);
	return ret;
}

/**
 * @brief write data to nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[in] pDataCtrl control writting data section: buffer, data lenght, ECC.
 * @param[in] pSpareCtrl control writting spare section: buffer, data lenght, ECC.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_writepage_ecc(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_PNAND_ECC_STRU  pDataCtrl,  T_PNAND_ECC_STRU pSpareCtrl)
{
    T_U32   i, loopCnt;
    T_BOOL  bSeparate_spare;
    T_U32   effective_size;
    T_U32   ecc_type;
    T_U32   total_len;
    T_U8    tmp_rowcycle = pNF_Add->RowCycle - 1;
    T_U32   ret;
    T_U32   status;
    T_U32   tmp = 0;
    T_U32   section_data_size;
    T_U32   *reg_addr;
    T_U8    l2_buf_id;
    T_U16   fail_cnt = 0;
    T_BOOL  bSpare_write;
    
    down(&nand_lock);
	#ifndef NAND_L2_ENABLE
    asm("AK_FlashDCache_zz:\n" "mrc  p15,0,r15,c7,c10,3\n" "bne AK_FlashDCache_zz");
	#endif	
    ak98_group_config(ePIN_AS_NFC);
    
 RETRY_WR: 
    ret = 0;
    bSpare_write = AK_FALSE;
    l2_buf_id = BUF_NULL;
    
    /*check whether is data and spare separate, get the length and ECC type of the first data seciton,
       and get the times to write data*/   
    bSeparate_spare = nf_check_data_spare_separate(pDataCtrl, pSpareCtrl, &loopCnt, &effective_size, &ecc_type);
	#if ZZ_DEBUG
	printk(KERN_INFO "zz nand_control.c %s:line %d phy_page=%d\n", __func__, __LINE__, RowAddr);
    printk(KERN_INFO "bSeparate_spare=%d loopCnt=%d effective_size=%d ecc_type=%d\n",
           bSeparate_spare,loopCnt,effective_size,ecc_type);
    #endif
    if (0 == loopCnt)
    {
        ret = 1;
        goto RETURN2;
    }    

    reg_addr = (T_U32 *)FLASH_CTRL_REG0; 

    if (NAND_512B_PAGE == pNF_Add->ChipType)
    {
        //... 需要调整指针
        if(ColumnAddr < 256)
        {
            REG32(reg_addr++) = (NFLASH_READ1 << 11) | COMMAND_CYCLES_CONF;
        }
        else if(ColumnAddr >= 512)
        {
            REG32(reg_addr++) = (NFLASH_READ22 << 11) | COMMAND_CYCLES_CONF;
        }
        else
        {
            REG32(reg_addr++) = (NFLASH_READ1_HALF << 11) | COMMAND_CYCLES_CONF;
        }
    }

    //sta_clr = 0,
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR; 
   
    //nandflash frame write command(NFLASH_FRAME_PROGRAM0)
    REG32(reg_addr++) = (NFLASH_FRAME_PROGRAM0 << 11) | COMMAND_CYCLES_CONF; 

    //nandflash send address
    reg_addr = nf_send_addr(reg_addr, ColumnAddr, RowAddr, pNF_Add->ColCycle, tmp_rowcycle);
    REG32(reg_addr) = ((RowAddr >> (tmp_rowcycle * 8)) << 11) | ADDRESS_CYCLES_CONF | LAST_CMD_FLAG;

    //execute cmd and wait command send done
    cmd_go(Chip);
    while ( !check_cmd_done() );
	#if ZZ_DEBUG
    printk(KERN_INFO "zz nand_control.c nand_writepage_ecc() 883 after while\n");
	#endif
	
#ifdef NAND_L2_ENABLE
    l2_buf_id = ak98_l2_alloc(ADDR_NFC);
    if (BUF_NULL == l2_buf_id)
    {
        ret = 1;
        goto RETURN2;
    } 
   // ak98_l2_dump_registers();
#else
    communicate_conf(NAND_FLASH, WRITE_BUF);
#endif

    for (i = 0; i < loopCnt; i++)
    {
        /*set data size and ecc type to write when writing spare section 
        if data and spare is separate */
        if (bSeparate_spare  && (( loopCnt - 1) == i) && (AK_NULL != pSpareCtrl))
        {
            bSpare_write = AK_TRUE;
            effective_size = pSpareCtrl->buf_len;
            ecc_type = pSpareCtrl->ecc_type;
        }

        //config ECC module
		
		tmp = ( ECC_CTL_ENC_EN | ECC_CTL_DIR_WRITE | ECC_CTL_ADDR_CLR	\
                | ECC_CTL_BYTE_CFG(effective_size) | ECC_CTL_NFC_EN | ECC_CTL_MODE(ecc_type));
        HAL_WRITE_UINT32(FLASH_ECC_REG0,  tmp);
        HAL_WRITE_UINT32(FLASH_ECC_REG0,  tmp|ECC_CTL_START);
        //sta_clr = 0,
        REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

        //config the whole length of a section including ecc part
        total_len = effective_size + nf_get_ecccode_len(ecc_type);
		#if ZZ_DEBUG
        printk(KERN_INFO "zz nand_control.c nand_writepage_ecc() 920 total_len=%d\n",
              total_len);
		#endif
        REG32(FLASH_CTRL_REG0) = ((total_len - 1) << 11) | WRITE_DATA_CONF | LAST_CMD_FLAG;

        //start to transfer data from memory to nand
        cmd_go(Chip);
		
        //copy page data to L2 buffer
        if(!bSpare_write)
        {
            //the length of a ecc data section
            if(!bSeparate_spare)
            {
                section_data_size = pDataCtrl->ecc_section_len - pSpareCtrl->buf_len;
            }
            else
            {
                section_data_size = pDataCtrl->ecc_section_len;
            }    
			
#ifdef NAND_L2_ENABLE            
                
                ak98_l2_combuf_cpu((T_U32)(pDataCtrl->buf + (i * section_data_size)), 
												l2_buf_id, section_data_size, MEM2BUF); 
            
            if(0 == (section_data_size & 63))
            {
                //wait nand controler transfer data
                while(0 != ak98_l2_get_status(l2_buf_id)); 
            }
           // ak98_l2_dump_registers();          
			
#else	// ! NAND_L2_ENABLE
            prepare_dat_send_cpu(pDataCtrl->buf+(i*section_data_size), section_data_size, NAND_FLASH);
#endif
        }

        //send spare data
        if ((AK_NULL != pSpareCtrl) && (!bSeparate_spare || bSpare_write))
        {
#ifdef NAND_L2_ENABLE
			#if ZZ_DEBUG
			printk(KERN_INFO "zz nand_control.c nand_writepage_ecc() 970 spare len=%d\n",pSpareCtrl->buf_len);
			#endif
            ak98_l2_combuf_cpu((T_U32)pSpareCtrl->buf, l2_buf_id, pSpareCtrl->buf_len, MEM2BUF);
#else
            prepare_dat_send_cpu(pSpareCtrl->buf + (i*NAND_FS_SIZE), NAND_FS_SIZE, NAND_FLASH);
#endif
        } 
       
        while (!check_cmd_done());
		
#ifdef NAND_L2_ENABLE
	    ak98_l2_clr_status(l2_buf_id);
#else
        //clear l2 status, because l2_combuf_cpu will set l2 status 
        ak98_l2_clr_status(WRITE_BUF);
#endif
        // wait for ECC complete
        do 
        {
            HAL_READ_UINT32(FLASH_ECC_REG0, tmp);       
        }   while(0 == (tmp & ECC_CTL_END));

        
        do
        {
            HAL_READ_UINT32(FLASH_ECC_REG0,tmp);
        }   while(0 == (tmp & ECC_CTL_ENC_RDY));
       
        HAL_WRITE_UINT32(FLASH_ECC_REG0, (tmp | ECC_CTL_END));
    }
	
#ifdef NAND_L2_ENABLE
    ak98_l2_free(ADDR_NFC);
#endif

    //sta_clr = 0,
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    reg_addr = (T_U32 *)FLASH_CTRL_REG0;

    //nandflash frame write command(NFLASH_FRAME_PROGRAM1)
    REG32(reg_addr++)= ((NFLASH_FRAME_PROGRAM1 << 11) | COMMAND_CYCLES_CONF | LAST_CMD_FLAG);

    //run command, wait cmd done
    cmd_go(Chip);
    while ( !check_cmd_done() );

    for ( i = 0; i < MAX_LOOP_CNT; i++ )
    {
        status = nf_read_chipstatus(Chip);
		#if ZZ_DEBUG
        printk(KERN_INFO "zz nand_control.c nand_writepage_ecc() 1014 status=0x%lx\n",
               status);
		#endif
        if ( 0 == (status & NFLASH_WRITE_PROTECT) )
        {
            while (1);
        }
        else if ( 0 != (status & NFLASH_HANDLE_READY) )
        {
            if ( 0 != (status & NFLASH_PROGRAM_SUCCESS) )
            {
                fail_cnt++;
                if (fail_cnt < 10)
                {
                    goto RETRY_WR;    //more than 10 ms to exit, otherwise continue
                }
                ret = 1;
                goto RETURN2;
            }
            else 
            {
                ret = 0;
                goto RETURN2;
            }
        }
        udelay(100);
    }
    ret = 2;
    
RETURN2:

    up(&nand_lock);
    return ret;
}

#if 0
/**
 * @brief write one page(page size>=2048) of data to nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[in] Data buffer for write data, should be large than or equal to 2048 bytes.
 * @param[in] Spare file system info.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_writesector_large(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 Data[], T_U32 Spare)
{
     return nand_writesector(Chip, RowAddr, ColumnAddr, pNF_Add, Data, Spare);
}

/**
 * @brief write one page(page size=512) of data to nand flash with ECC .
 *
 * @author jiangdihui
 * @date 2010-07-23
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[in] Data buffer for write data, should be 512 bytes.
 * @param[in] Spare file system info.
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_writesector(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, T_U8 Data[], T_U8 *Spare)
{
    T_NAND_ECC_STRU data_ctrl;
    T_NAND_ECC_STRU spare_ctrl;

    data_ctrl.buf = Data;
    data_ctrl.buf_len = pNF_Add->PageSize;
    data_ctrl.ecc_section_len = NAND_DATA_SIZE + NAND_FS_SIZE;
    data_ctrl.ecc_type = pNF_Add->EccType;

    spare_ctrl.buf = Spare + 2;
    spare_ctrl.buf_len = NAND_FS_SIZE;
    spare_ctrl.ecc_section_len = NAND_DATA_SIZE + NAND_FS_SIZE;
    spare_ctrl.ecc_type = pNF_Add->EccType;

    return nand_writepage_ecc(Chip, RowAddr, ColumnAddr, pNF_Add, &data_ctrl, &spare_ctrl);
}

#endif
/**
 * @brief write data to nandflash without ECC.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be read.
 * @param[in] RowAddr the row address of nandflash.
 * @param[in] ColumnAddr the column address of nandflash.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @param[in] Data buffer for write data.
 * @param[in] Len how many bytes write to nandflash
 * @return  T_U32
 * @retval  0 success
 */
T_U32 nand_writebytes(T_U32 Chip, T_U32 RowAddr, T_U32 ColumnAddr, T_PNandflash_Add pNF_Add, const T_U8 Data[], T_U32 Len)
{
    T_U32 *reg_addr;
    T_U32 ret = 0, LoopCnt = 0, tmp, i, status;
    T_U8  tmp_rowcycle = pNF_Add->RowCycle - 1, buf_id = BUF_NULL;
    T_U16 fail_cnt = 0;

    down(&nand_lock);

RETRY_WR:
    i = 0;
    reg_addr = (T_U32 *)FLASH_CTRL_REG0;

    // sta_clr = 0,
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;
    
    //nandflash frame write command(NFLASH_FRAME_PROGRAM0)
    REG32(reg_addr++)=((NFLASH_FRAME_PROGRAM0 << 11) | COMMAND_CYCLES_CONF); 

    //nandflash send address
    reg_addr = nf_send_addr(reg_addr, ColumnAddr, RowAddr, pNF_Add->ColCycle, tmp_rowcycle);
    REG32(reg_addr)=(((RowAddr >> (tmp_rowcycle * 8)) << 11) | ADDRESS_CYCLES_CONF | LAST_CMD_FLAG);

    //run cmd, wait fifo command send done
    cmd_go(Chip);
    while ( !check_cmd_done() );

    buf_id = ak98_l2_alloc(ADDR_NFC);
    if (BUF_NULL == buf_id)
    {
        ret = 1;
        goto RETURN4;
    }
        
    //config ECC module
    tmp = ( ECC_CTL_DIR_WRITE | ECC_CTL_ADDR_CLR    \
           | ECC_CTL_BYTE_CFG(Len) | ECC_CTL_NFC_EN);
    HAL_WRITE_UINT32(FLASH_ECC_REG0,  tmp);
    HAL_WRITE_UINT32(FLASH_ECC_REG0,  tmp|ECC_CTL_START);

    REG32(FLASH_CTRL_REG0)=(((Len -  1) << 11) | WRITE_DATA_CONF | LAST_CMD_FLAG);

    cmd_go(Chip);

    //copy data to l2
    for(i = 0; i < (Len / NAND_DATA_SIZE); i++)
    {
        ak98_l2_combuf_cpu((T_U32)(Data + (i * NAND_DATA_SIZE)), buf_id, 
													NAND_DATA_SIZE, MEM2BUF);
        //wait nand controler transfer data
        while(0 != ak98_l2_get_status(buf_id));
    }
    
    tmp = Len & 511;
    if (tmp != 0)
    {
        ak98_l2_combuf_cpu((T_U32)(Data + i * NAND_DATA_SIZE), buf_id, tmp, MEM2BUF);
    }

    while ( !check_cmd_done());

    ak98_l2_clr_status(buf_id);
   
    //wait for ECC complete        
    do 
    {
        HAL_READ_UINT32(FLASH_ECC_REG0, tmp);
    }while ((tmp  & ECC_CTL_END) != ECC_CTL_END);
    HAL_WRITE_UINT32(FLASH_ECC_REG0, (tmp | ECC_CTL_END));

    reg_addr = (T_U32 *)FLASH_CTRL_REG0;
    REG32(reg_addr++)= ((NFLASH_FRAME_PROGRAM1 << 11) | COMMAND_CYCLES_CONF);
    REG32(reg_addr) = WAIT_JUMP_CONF | LAST_CMD_FLAG;// wait R/B rising edge

    cmd_go(Chip);
    while ( !check_cmd_done() );
 
    for ( LoopCnt = 0; LoopCnt < MAX_LOOP_CNT; LoopCnt++)
    {
        status = nf_read_chipstatus(Chip);
        if ( 0 == (status & NFLASH_WRITE_PROTECT) )
        {
            while (1);
        }
        else if ( 0 != (status & NFLASH_HANDLE_READY) )
        {
            if ( 0 != (status & NFLASH_PROGRAM_SUCCESS) )
            {
                fail_cnt++;
                if (fail_cnt < 10)
                {
                    goto RETRY_WR;    //more than 10 ms to exit, otherwise continue
                }
                ret = 1;
                goto RETURN4;
            }
            else 
            {
                ret = 0;
                goto RETURN4;
            }
        } 
        udelay(100);
    }
    ret = 2;

RETURN4:
    ak98_l2_free(ADDR_NFC);
    up(&nand_lock);
    return ret;
}

/**
 * @brief erase one block of nandflash.
 *
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be operated.
 * @param[in] BlkStartPage first page of the block.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @return T_U32
 * @retval 0 means write successfully
 * @retval 1 means write unsuccessfully
 * @retval 2 means time out
 */
T_U32 nand_eraseblock(T_U32 Chip, T_U32 BlkStartPage, T_PNandflash_Add pNF_Add)
{
    T_U32 status;
    T_U32 *reg_addr;
    T_U32 ret = 0;
    T_U32 LoopCnt  = 0;
    T_U16 fail_cnt = 0;

   // printk(KERN_INFO "zz nand_control.c nand_eraseblock() 1301 startpage=%d\n", 
   //        BlkStartPage);

    down(&nand_lock);
 	
    ak98_group_config(ePIN_AS_NFC);
 	
RETRY_EB:
    reg_addr = (T_U32 *)FLASH_CTRL_REG0;

    // sta_clr = 0, need?
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    //nandflash erase command
    REG32(reg_addr++) = (NFLASH_BLOCK_ERASE0 << 11) | COMMAND_CYCLES_CONF;

    //nandflash physic page address
    reg_addr = nf_send_addr(reg_addr, 0, BlkStartPage, 0, pNF_Add->RowCycle);	 	

    //nandflash erase command
    REG32(reg_addr++) = (NFLASH_BLOCK_ERASE1 << 11) | COMMAND_CYCLES_CONF;

    REG32(reg_addr) = WAIT_JUMP_CONF | LAST_CMD_FLAG;// wait R/B rising edge

    // excute operation, CE1, enable power saving, CE# keep LOW wait R/B
    cmd_go(Chip);
	
    // wait end & read data (data at where)
    while ( !check_cmd_done() );   // 等待操作完成
	
    for ( LoopCnt = 0; LoopCnt <MAX_LOOP_CNT; LoopCnt ++ )
    {
        status = nf_read_chipstatus(Chip);
        if ( 0 == (status & NFLASH_WRITE_PROTECT) )
        {
        	#if ZZ_DEBUG
            printk(KERN_INFO "zz nand_control.c nand_eraseblock() 1275\n");
			#endif
            while (1);
        }
        else if ( 0 != (status & NFLASH_HANDLE_READY) )
        {
            if ( 0 != (status & NFLASH_PROGRAM_SUCCESS) )
            {
                fail_cnt++;
                if (fail_cnt < 2)
                {
                    goto RETRY_EB;
                }
                ret = 1;
                goto RETURN5;
            }
            else
            {
                ret = 0;
                goto RETURN5;
            }
        }
        udelay(100);
    }
    ret = 2;
RETURN5:
    #ifdef CONFIG_MTD_NAND_TEST  
    T_U32 blocknum = 0;
    blocknum = BlkStartPage/pNF_Add->PagesPerBlock;
    *(nand_erase_test + blocknum) += 1;
   // printk("hy erase_count[%u]=%d\n",blocknum, *(nand_erase_test + blocknum));
    #endif

    up(&nand_lock);

    return ret;
}

#ifdef CONFIG_MTD_NAND_TEST  
T_U32 nand_get_erase_block_count(T_U32 blocknum)
{   
    if (blocknum >= nand_total_blknum)
    {
        printk("blocknum is biger than total num\n");
        return -1;
    }
    else
    {
        return *(nand_erase_test + blocknum);
    }
}
#endif

/**
 * @brief copy one physical page to another one.
 *
 * hardware copyback mode, there should be caches in nandflash, source and destation page should be in the same plane
 * @author yiruoxiang
 * @date 2006-11-02
 * @param[in] Chip which chip will be operated.
 * @param[in] SouPhyPage the source page to read.
 * @param[in] DesPhyPage the destination page to write.
 * @param[in] pNF_Add information of the nandflash characteristic.
 * @return  T_U32
 * @retval  0 means write successfully
 * @retval  1 means write unsuccessfully
 * @retval  2 means time out
 */
T_U32 nand_copyback(T_U32 Chip, T_U32 SrcPhyPage, T_U32 DestPhyPage, T_PNandflash_Add pNF_Add)
{
    T_U32 status;
    T_U32 *reg_addr;
    T_U32 ret = 0;
    T_U16 fail_cnt = 0;
    T_U32 i;

    down(&nand_lock);

RETRY_CP:
    reg_addr = (T_U32 *)FLASH_CTRL_REG0;

    // sta_clr = 0,
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;
    //nandflash read1 command
    REG32(reg_addr++) = (NFLASH_READ1 << 11) | COMMAND_CYCLES_CONF;

    reg_addr = nf_send_addr(reg_addr, 0, SrcPhyPage,  pNF_Add->ColCycle, pNF_Add->RowCycle);

    //nandflash copyback read command
    if (NAND_512B_PAGE != pNF_Add->ChipType)
    {
        REG32(reg_addr++) = (NFLASH_COPY_BACK_READ << 11) | COMMAND_CYCLES_CONF;
    }    

    REG32(reg_addr++) = WAIT_JUMP_CONF;// wait R/B rising edge

    //nandflash copyback write command
    if (NAND_512B_PAGE != pNF_Add->ChipType)
    {
        REG32(reg_addr++) = (NFLASH_COPY_BACK_WRITE << 11) | COMMAND_CYCLES_CONF;
    }
    else
    {
        REG32(reg_addr++) = (NFLASH_COPY_BACK_WRITE1 << 11) | COMMAND_CYCLES_CONF;
    }

    reg_addr = nf_send_addr(reg_addr, 0, DestPhyPage,  pNF_Add->ColCycle, pNF_Add->RowCycle);

    //nandflash copyback confirm command
    //if (NFLASH_SMALL_PAGE != pNF_Add->ChipType)
    //small page slc also need to send '10H' command, resumed on 20070615
    REG32(reg_addr++) = (NFLASH_COPY_BACK_CONFIRM << 11) | COMMAND_CYCLES_CONF;
    
    REG32(reg_addr) = WAIT_JUMP_CONF | LAST_CMD_FLAG;// wait R/B rising edge
    
    cmd_go(Chip);

    // wait reg22 fifo command send done
    while ( !check_cmd_done() );

    for ( i = 0; i < MAX_LOOP_CNT; i++ )
    {
        status = nf_read_chipstatus(Chip);
        if ( 0 != (status & NFLASH_HANDLE_READY) )
        {
            if ( 0 != (status & NFLASH_PROGRAM_SUCCESS) )
            {
                fail_cnt++;
                if (fail_cnt < 10)
                {
                    goto RETRY_CP;    //more than 10 ms to exit, otherwise continue
                }
                ret = 1;
                goto RETURN6;
            }
            else
            {
                ret = 0;
                goto RETURN6;
            }
        }
        udelay(100);//delay or handle other task
    }
    ret = 2;
RETURN6:
    up(&nand_lock);
    return ret;
}

//***********************************************************************************
/*
static T_VOID nf_controller_reset(T_VOID)
{
    T_U32 cmd_len = 0xf5ad1;
    T_U32 dat_len = 0xf5c5c;
    
    //akprintf(C3, M_DRVSYS, "reset nand controller\r\n");  
    
    cmd_len = REG32(FLASH_CTRL_REG23);
    dat_len = REG32(FLASH_CTRL_REG24);
    
    sysctl_reset(RESET_NANDFLASH);

    REG32(FLASH_CTRL_REG23) = cmd_len;
    REG32(FLASH_CTRL_REG24) = dat_len;
}
*/
//***********************************************************************************
static T_U8 nf_read_chipstatus(T_U32 Chip)
{
    T_U32 status;

    //sta_clr = 0,
    REG32(FLASH_CTRL_REG22) &= ~NF_REG_CTRL_STA_STA_CLR;

    REG32(FLASH_CTRL_REG0 + 0x00) = (NFLASH_STATUS_READ << 11) | COMMAND_CYCLES_CONF;

    //read status,1byte
    REG32(FLASH_CTRL_REG0 + 0x04) = (0 << 11) | READ_INFO_CONF | LAST_CMD_FLAG;

    // excute operation, CE1, enable power saving, CE# keep LOW wait R/B
    //must open write protect
    cmd_go(Chip);

    // wait end & read data (data at where)
    while ( !check_cmd_done() );   // 等待操作完成

    // read status
    status = REG32(FLASH_CTRL_REG20);

    return((T_U8)(status & 0xFF));
}

//***********************************************************************************
static T_U32 *nf_send_addr(T_U32 *reg_addr, T_U32 ColumnAddr,T_U32 rowAddr, T_U32 col_cycle, T_U32 row_cycle)
{
    T_U8 cycle,value;

    //send column address
    for (cycle = 0; cycle < col_cycle; cycle++)
    {
        value = (ColumnAddr >> (8 * cycle)) & 0xFF;
        REG32(reg_addr++) = (value << 11) | ADDRESS_CYCLES_CONF;
    }

    //send row address
    for (cycle = 0; cycle < row_cycle; cycle++)
    {
        value = (rowAddr >> (8 * cycle)) & 0xFF;
        REG32(reg_addr++) = (value << 11) | ADDRESS_CYCLES_CONF;
    }

    return reg_addr;
}
//***********************************************************************************
static T_BOOL nf_check_data_spare_separate(T_PNAND_ECC_STRU pDataCtrl, T_PNAND_ECC_STRU pSpareCtrl, T_U32 *cycle, T_U32 *effective_size, T_U32 *ecc_type)
{
    T_BOOL ret = AK_TRUE;

    if(AK_NULL == pDataCtrl)
    {
        *cycle = 1;
        *effective_size = pSpareCtrl->buf_len;
        *ecc_type = pSpareCtrl->ecc_type;
    }
    else
    {
        T_U32 section_len = pDataCtrl->ecc_section_len;

        if(0 != (pDataCtrl->buf_len % section_len)) //spare and data is not separate
        {
            section_len -= pSpareCtrl->buf_len;

            if(0 != (pDataCtrl->buf_len % section_len))  //param err
            {
                *cycle = 0;
            }    
            else
            {
                *cycle = pDataCtrl->buf_len / section_len;
            }

            ret = AK_FALSE;
        }
        else
        {
            *cycle = pDataCtrl->buf_len / section_len;

            if(AK_NULL != pSpareCtrl)
            {
                (*cycle)++;
            }
        }

        *effective_size = pDataCtrl->ecc_section_len;
        *ecc_type = pDataCtrl->ecc_type;
    }

    return ret;
}    
//***********************************************************************************
static T_U32 nf_get_ecccode_len(T_U32 ecc_type)
{
    T_U32 ecc_len = NAND_PARITY_SIZE_MODE0;
    
    switch(ecc_type)
    {
        case ECC_4BIT_P512B:
            ecc_len = NAND_PARITY_SIZE_MODE0;
            break;
        case ECC_8BIT_P512B:
            ecc_len = NAND_PARITY_SIZE_MODE1;
            break;            
        case ECC_12BIT_P512B:
            ecc_len = NAND_PARITY_SIZE_MODE2;
            break;
        case ECC_16BIT_P512B:
            ecc_len = NAND_PARITY_SIZE_MODE3;
            break;    
        case ECC_24BIT_P1KB:
            ecc_len = NAND_PARITY_SIZE_MODE4;
            break;
        case ECC_32BIT_P1KB:
            ecc_len = NAND_PARITY_SIZE_MODE5;
            break;    
        default:
            break; 
    } 

    return ecc_len;
} 

//***********************************************************************************
static T_U8 nf_check_ecc_status(T_U32 stat)
{
    if ( (stat & ECC_CHECK_NO_ERROR) == ECC_CHECK_NO_ERROR )
    {
        stat |= ECC_CHECK_NO_ERROR;
        HAL_WRITE_UINT32(FLASH_ECC_REG0 ,stat);
        HAL_READ_UINT32(FLASH_ECC_REG0 ,stat);
        return DATA_ECC_CHECK_OK;
    }
    else if ( (stat & ECC_ERROR_REPAIR_CAN_NOT_TRUST) == ECC_ERROR_REPAIR_CAN_NOT_TRUST)
    {
        stat |= ECC_ERROR_REPAIR_CAN_NOT_TRUST;
        HAL_WRITE_UINT32(FLASH_ECC_REG0 ,stat);
        HAL_READ_UINT32(FLASH_ECC_REG0 ,stat);
        return DATA_ECC_ERROR_REPAIR_CAN_NOT_TRUST;
    }
    else
    {
        return DATA_ECC_ERROR_REPAIR_CAN_TRUST;
    }
}

//***********************************************************************************
static T_U32 nf_check_repair_ecc(T_U8 *pMain_buf, T_U8 *pAdd_buf, 
                    T_U32 section_size, T_U32 add_len, T_U32 ecc_type, T_BOOL  bSeparate_spare)
{
    T_U32   tmp,m,j;
    T_U32   position_info;
    T_U32   error_count;
    T_U32   bit_errs;
    T_U8    max_errcnt;
    T_U8    ecc_stat; 

    //wait for ECC complete    
    do
    {
        HAL_READ_UINT32(FLASH_ECC_REG0,tmp);
    } while(0 == (tmp & ECC_CTL_END));
    
	do
    {
        HAL_READ_UINT32(FLASH_ECC_REG0,tmp);
    } while(0 == (tmp & ECC_CTL_DEC_RDY));
    
    HAL_WRITE_UINT32(FLASH_ECC_REG0, (tmp | ECC_CTL_END));

    ecc_stat = nf_check_ecc_status(tmp);

    switch(ecc_stat)
    {
        case DATA_ECC_CHECK_OK:
            break;

        case DATA_ECC_ERROR_REPAIR_CAN_TRUST:        
            s_flip_bits = 0;
            
            if(ecc_type >= ECC_24BIT_P1KB)
            {
                max_errcnt = (ecc_type - 1) * 8;
            } 
            else
            {
                max_errcnt = (ecc_type + 1) * 4;
            }

            //ecc correct
            for (error_count = 0; error_count < max_errcnt; error_count++)
            {  
                HAL_READ_UINT32(FLASH_ECC_REPAIR_REG0 + error_count * 4 ,position_info);
                ecc_repair(position_info, pMain_buf, pAdd_buf, section_size, add_len, ecc_type);
            }
            break;

        case DATA_ECC_ERROR_REPAIR_CAN_NOT_TRUST:
            bit_errs = 0;
            if (!bSeparate_spare)
            {
                for(j = 0; j < add_len; j++)
                {
                    if (0xff != *(pAdd_buf+ j))
                    {
                        for(m = 0; m < 8; m++)
                        {
                            if(0 == ((*(pAdd_buf + j)) & (1 << m)))
                            {
                                bit_errs++;
                            }    
                        }

                        if(bit_errs > 3)
                        {
                            goto RETURN1;
                        }
                    }
                } 

                if(0 != bit_errs)
                {
                    memset(pAdd_buf, 0xFF, add_len);
				}
           }
           else
           {
                for(j = 0; j < section_size; j++)
                {
                    if (0xff != *(pMain_buf + j))
                    {
                        for(m = 0; m < 8; m++)
                        {
                            if(0 == ((*(pMain_buf + j)) & (1 << m)))
                            {
                                bit_errs++;
                            }    
                        }

                        if(bit_errs > 3)
                        {
                            goto RETURN1;
                        }
                    }
                }

                if(0 != bit_errs)
                {
                    memset(pMain_buf, 0xFF, section_size);
                }    
           }
   
           break;
    }
    
    return ecc_stat;
    
RETURN1:
    return DATA_ECC_CHECK_ERROR;  
}

static T_U8 bitnum(T_U32 i)
{
    T_U8 ret = 0;
    for(; (T_U32)(1<<ret) < i; ret++);

    return ret;
}

static T_U32 ecc_repair(T_U32 wrong_info, T_U8 *main_buf, T_U8 *add_buf, T_U32 main_size, T_U32 add_size, T_U32 ecc_type)
{
    
    T_U32 position = (wrong_info >> 10) & 0xfff;    
    T_U32 offset;
    T_U32 byte_index;
    T_U8  correct;
    T_U32 shift;
    T_U32 ecccode_len = nf_get_ecccode_len(ecc_type);
    T_U32 data_whole_len = main_size + add_size;

    data_whole_len += ecccode_len;

    if (position)
    {
        // There are chances that two errors fall into contiguous bits. Then position will have two bits set.
        // For each bit of position
        for (shift=0; shift<12; shift++)
        {
            if (0 == (position & (1<<shift)))
            {
                // no error in this block
                continue;
            }
            
           
#if 1  
           
            byte_index = data_whole_len + 256 * (((bitnum(position & (1 << shift))) - 6) >> 1) - 3 * 256;
#else
            switch (position & (1<<shift))
            {
            case (1<<0):
            case (1<<1):
                // block1
                break;

            case (1<<2):
            case (1<<3):
                // block2
                byte_index = data_whole_len - 5 * 256;
                break;

            case (1<<4):
            case (1<<5):
                // block3
                byte_index = data_whole_len - 4 * 256;
                break;

            case (1<<6):
            case (1<<7):
                // block4
                byte_index = data_whole_len - 3 * 256;
                break;

            case (1<<8):
            case (1<<9):
                // block5
                byte_index = data_whole_len - 2 * 256;
                break;

            case (1<<10):
            case (1<<11):
                // block6
                byte_index = data_whole_len - 1 * 256;
                break;
           
            default:
                while (1);
                break;
            }
            
            
#endif
            offset = 2 * (wrong_info & 0x3ff) + (((1 << shift) & 0xaaa) ? 1 : 0);            

            byte_index += offset>>3;
            correct = 1 << (7 - (offset&0x7));
            
#ifdef ONLY_WEAK_DANGER_PRINT
            s_ecc_info.err_loc[s_flip_bits] = byte_index;
            //akprintf(C3, M_DRVSYS, "byte=%d,s_flip_bits=%d ", s_ecc_info.err_loc[s_flip_bits], s_flip_bits);
#else
            printk(KERN_INFO "byte=%d ", byte_index);
#endif

            if (byte_index < main_size)
            {
				// error occurs in data
				
#ifdef ONLY_WEAK_DANGER_PRINT
                s_ecc_info.bad_bytes[s_flip_bits] = main_buf[byte_index];
                main_buf[byte_index] ^= correct;
                s_ecc_info.good_bytes[s_flip_bits] = main_buf[byte_index];
#else
                main_buf[byte_index] ^= correct;
#endif
            }
            else if (byte_index < (main_size+add_size))
            {
				// error occurs in file system info
				
#ifdef ONLY_WEAK_DANGER_PRINT
                s_ecc_info.bad_bytes[s_flip_bits] = add_buf[byte_index - main_size];
                add_buf[byte_index - main_size] ^= correct;
                s_ecc_info.good_bytes[s_flip_bits] = add_buf[byte_index - main_size];
#else
                add_buf[byte_index - main_size] ^= correct;
#endif
            }
            else
            {// error occurs in parity data
                // nothing to do            
            
#ifdef ONLY_WEAK_DANGER_PRINT
                s_ecc_info.good_bytes[s_flip_bits] = s_ecc_info.bad_bytes[s_flip_bits];
#endif
            }

            s_flip_bits++;
            
        }
    }
    return 1;
}

//************************************************************************************
/**
* @BRIEF    check cmd_done bit
* @AUTHOR   lgj
* @MODIFY   yiruoxiang
* @DATE     2006-7-17
* @PARAM    T_VOID
* @RETURN
* @RETVAL
*/
static T_BOOL check_cmd_done(T_VOID)
{
    volatile unsigned long status;
        
    status = *(volatile unsigned long *)(FLASH_CTRL_REG22);
   // printk(KERN_INFO "zz check_cmd_done status=0x%x\n",status);

    if ( 0 != (REG32(FLASH_CTRL_REG22) & BIT_DMA_CMD_DONE) )
        return AK_TRUE;
    else
        return AK_FALSE;
}

//*****************************************************************************************
/**
 * @brief: start command sequence
 */
static T_VOID cmd_go(T_U32 Chip)
{
    volatile T_U32 status;

    status = REG32(FLASH_CTRL_REG22);
    status &= ~( NF_REG_CTRL_STA_CMD_DONE | NF_REG_CTRL_STA_CE0_SEL | NF_REG_CTRL_STA_CE1_SEL); //remove CE flag
    status |= NCHIP_SELECT(Chip) | DEFAULT_GO;
    REG32(FLASH_CTRL_REG22) = status;
}

//*****************************************************************************************
static T_U32 calc_new(T_U8 data_opt_len, T_U8 data_we_fe, T_U8 data_we_re, T_U8 data_re_fe, T_U8 data_re_re)
{
    T_U32 dat_len;
    
    if (data_opt_len < 3)
    {
        data_opt_len = 3;
    }
    
    if (data_opt_len == data_we_re)
    {
        data_we_re--;
    }
    
    if (data_opt_len == data_re_re)
    {
        data_re_re--;
    }

    dat_len = (data_opt_len << 16) | (data_we_fe << 12) | (data_we_re << 8)
              | (data_re_fe << 4) | (data_re_re << 0);

	if (data_opt_len > 0xf)
	{
		printk("Large than 0xf\n");
		dat_len = 0xF5C5C;
	}

    return dat_len;
}

