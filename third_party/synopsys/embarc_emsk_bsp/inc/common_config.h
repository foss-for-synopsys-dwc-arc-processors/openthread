/* ------------------------------------------
 * Copyright (c) 2016, Synopsys, Inc. All rights reserved.

 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:

 * 1) Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.

 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.

 * 3) Neither the name of the Synopsys, Inc., nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * \version 2016.05
 * \date 2014-12-25
 * \author Wayne Ren(Wei.Ren@synopsys.com)
--------------------------------------------- */

/**
 * \file
 * \ingroup	COMMON_CONFIG
 * \brief	header file to define common definitions error management
 */

/**
 * \addtogroup	COMMON_CONFIG
 * @{
 */

#ifndef _COMMON_CONFIG_H_
#define _COMMON_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * OSP Definition
 ****************************************************************************/
/**
 *  Toolchain Definition for MetaWare or GNU
 */
#define __MW__
// #define __GNU__

/**
 *  Use task_main in embARC Startup process. See board/board.c for details.
 */
#define EMBARC_USE_BOARD_MAIN

/**
 *  Must be set.
 *  If changed, modify .lcf file for
 *	.stack ALIGN(4) SIZE(524288): {}
 *	.heap? ALIGN(4) SIZE(524288): {}
 */
#define _STACKSIZE			524288
#define _HEAPSZ				524288



#ifdef __cplusplus
}
#endif

#endif /* _COMMON_CONFIG_H_ */
/** @} end of group COMMON_CONFIG */
