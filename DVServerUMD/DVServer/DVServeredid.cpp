﻿/*===========================================================================
; DVServeredid.cpp
;----------------------------------------------------------------------------
; Copyright (C) 2021 Intel Corporation
; SPDX-License-Identifier: MS-PL
;
; File Description:
;   This file sends edid ioctl request to kmd & trim the unsupported modelist
;--------------------------------------------------------------------------*/

#include "DVServeredid.h"
#include<DVServeredid.tmh>

using namespace Microsoft::IndirectDisp;
PSP_DEVICE_INTERFACE_DETAIL_DATA device_iface_edid_data;
struct edid_info* edata = NULL;
struct screen_info* mdata = NULL;
ULONG bytesReturned = 0;

unsigned int blacklisted_resolution_list[][2] = { {1400,1050} }; // blacklisted resolution can be appended here
char* screenedid[MAX_SCAN_OUT] = { "ScreenEDID1", "ScreenEDID2", "ScreenEDID3", "ScreenEDID4" };

/*******************************************************************************
*
* Description
*
* get_edid_data - This function gets the handle of DVserverKMD edid device node
* and send an ioctl(IOCTL_DVSERVER_GET_EDID_DATA)to DVserverKMD to get edid data
*
* Parameters
* Device frame Handle to DVServerKMD
* pointer to IndirectSampleMonitor structure
* Screen ID
*
* Return val
* int - 0 == SUCCESS, -1 = ERROR
*
******************************************************************************/
int get_edid_data(HANDLE devHandle, void* m, DWORD id)
{
	TRACING();
	IndirectSampleMonitor* monitor = (IndirectSampleMonitor*)m;
	wchar_t* Lscreenedid;
	BYTE regedid[EDID_SIZE] = { 0 };
	ULONG buff_size = EDID_SIZE;
	unsigned int i = 0, edid_mode_index = 0, modeSize;
	size_t requiredSize = 0;
	int status = 0;

	if (!devHandle || !m) {
		ERR("Invalid parameter\n");
		return DVSERVERUMD_FAILURE;
	}

	edata = (struct edid_info*)malloc(sizeof(struct edid_info));
	if (edata == NULL) {
		ERR("Failed to allocate edid structure\n");
		return DVSERVERUMD_FAILURE;
	}
	SecureZeroMemory(edata, sizeof(struct edid_info));
	edata->screen_num = id;

	DBGPRINT("Requesting Mode List size through EDID IOCTL\n");
	if (!DeviceIoControl(devHandle, IOCTL_DVSERVER_GET_EDID_DATA, edata, sizeof(struct edid_info), edata, sizeof(struct edid_info), &bytesReturned, NULL)) {
		ERR("IOCTL_DVSERVER_GET_EDID_DATA call failed\n");
		free(edata);
		return DVSERVERUMD_FAILURE;
	}
	if (edata->mode_size > MODE_LIST_MAX_SIZE) {
		ERR("Invalid id \n");
		free(edata);
		return DVSERVERUMD_FAILURE;
	}

	/*Resetting the edata buffer for coverity*/
	modeSize = edata->mode_size;
	SecureZeroMemory(edata, sizeof(struct edid_info));
	edata->screen_num = id;
	edata->mode_size = modeSize;

	edata->mode_list = (struct mode_info*)malloc(sizeof(struct mode_info) * edata->mode_size);
	if (edata->mode_list == NULL) {
		ERR("Failed to allocate mode list structure\n");
		free(edata);
		return DVSERVERUMD_FAILURE;
	}
	SecureZeroMemory(edata->mode_list, sizeof(struct mode_info) * edata->mode_size);

	DBGPRINT("Requesting EDID Data through EDID IOCTL\n");
	if (!DeviceIoControl(devHandle, IOCTL_DVSERVER_GET_EDID_DATA, edata, sizeof(struct edid_info), edata, sizeof(struct edid_info), &bytesReturned, NULL)) {
		ERR("IOCTL_DVSERVER_GET_EDID_DATA call failed!\n");
		free(edata->mode_list);
		free(edata);
		return DVSERVERUMD_FAILURE;
	}
	/*Rechecking for possible out of range */
	if (edata->mode_size > MODE_LIST_MAX_SIZE) {
		ERR("mode list is corrupted \n");
		free(edata->mode_list);
		free(edata);
		return DVSERVERUMD_FAILURE;
	}

	try {
		Lscreenedid = new wchar_t[strlen(screenedid[id]) + 1];
	}
	catch (const std::bad_alloc e) {
		ERR("Mode list size is Modified \n");
		free(edata->mode_list);
		free(edata);
		return DVSERVERUMD_FAILURE;
	}
	mbstowcs_s(&requiredSize, Lscreenedid, strlen(screenedid[id]) + 1, screenedid[id], strlen(screenedid[id]));
	if (requiredSize != 0) {
		DBGPRINT("Screen EDID regkey = %ls", Lscreenedid);
		status = read_dvserver_registry_binary(Lscreenedid, regedid, &buff_size);

		if (status != DVSERVERUMD_SUCCESS) {
			DBGPRINT("idd_read_registry_binary Failed, EDID received from KMD will be used");
			write_dvserver_registry_binary(Lscreenedid, edata->edid_data, EDID_SIZE);
			memcpy_s(monitor->pEdidBlock, monitor->szEdidBlock, edata->edid_data, monitor->szEdidBlock);
		} else {
			DBGPRINT("idd_read_registry_binary Passed, EDID from local registry will be used");
			memcpy_s(monitor->pEdidBlock, monitor->szEdidBlock, regedid, monitor->szEdidBlock);
		}
	}
	delete[] Lscreenedid;

	monitor->ulPreferredModeIdx = 0;

	DBGPRINT("Modes\n");
	for (i = 0; i < edata->mode_size; i++) {
		//TRIMMING LOGIC:
		//Restricting EDID size to 32
		//Discarding modes with width more than 3840 & less than 1024
		//Discarding blacklisted modes
		//Discarding the modes for which kmd & umd strides are not equal
		if ((edata->mode_list[i].width <= WIDTH_UPPER_CAP) &&
			(edata->mode_list[i].width >= WIDTH_LOWER_CAP) &&
			(edid_mode_index < monitor->szModeList) &&
			(is_blacklist(edata->mode_list[i].width, edata->mode_list[i].height) == 0) &&
			(is_samestride(edata->mode_list[i].width) == 0)) {
			monitor->pModeList[edid_mode_index].Width = edata->mode_list[i].width;
			monitor->pModeList[edid_mode_index].Height = edata->mode_list[i].height;
			if ((DWORD)edata->mode_list[i].refreshrate == REFRESH_RATE_59)
				monitor->pModeList[edid_mode_index].VSync = REFRESH_RATE_60;
			else
				monitor->pModeList[edid_mode_index].VSync = (DWORD)edata->mode_list[i].refreshrate;
			DBGPRINT("[%d]: %dx%d@%d\n", edid_mode_index,
				monitor->pModeList[edid_mode_index].Width, monitor->pModeList[edid_mode_index].Height,
				monitor->pModeList[edid_mode_index].VSync);
			edid_mode_index++;
		}
	}
	free(edata->mode_list);
	free(edata);
	return DVSERVERUMD_SUCCESS;
}

/*******************************************************************************
*
* Description
*
* get_total_screens - This function sends an ioctl(IOCTL_DVSERVER_GET_TOTAL_SCREENS)
* to DVserverKMD to get the total number of screens
*
* Parameters
* Device frame Handle to DVServerKMD
*
* Return val
* int - -1 = ERROR, any other value = number of total screens connected
*
******************************************************************************/
int get_total_screens(HANDLE devHandle)
{
	TRACING();
	int ret = DVSERVERUMD_FAILURE;

	if (!devHandle) {
		ERR("Invalid devHandle\n");
		return ret;
	}

	mdata = (struct screen_info*)malloc(sizeof(struct screen_info));
	if (mdata == NULL) {
		ERR("Failed to allocate Screen structure\n");
		return ret;
	}
	SecureZeroMemory(mdata, sizeof(struct screen_info));

	DBGPRINT("Requesting Screen Count through Screen IOCTL\n");
	if (!DeviceIoControl(devHandle, IOCTL_DVSERVER_GET_TOTAL_SCREENS, mdata, sizeof(struct screen_info), mdata, sizeof(struct screen_info), &bytesReturned, NULL)) {
		ERR("IOCTL_DVSERVER_GET_TOTAL_SCREENS call failed\n");
		free(mdata);
		return ret;
	}

	DBGPRINT("Total screens = %d\n", mdata->total_screens);
	ret = mdata->total_screens;
	free(mdata);
	return ret;
}

/*******************************************************************************
*
* Description
*
* is_blacklist - This function blacklists the resolution it receives from
* DVserverKMD comparing it from the unsuported reolutions formats which get
* recieved from Qemu.
*
* Parameters
* width - resolution width
* height - resolution height
*
* Return val
* int - 0 == SUCCESS, -1 = ERROR
*
******************************************************************************/
int is_blacklist(unsigned int width, unsigned int height)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(blacklisted_resolution_list); i++) {
		if ((width == blacklisted_resolution_list[i][0]) && (height == blacklisted_resolution_list[i][1])) {
			return DVSERVERUMD_FAILURE;
		}
	}
	return DVSERVERUMD_SUCCESS;
}

/*******************************************************************************
*
* Description
*
* is_samestride - This function generates kmd, umd strides & compare them
* It returns SUCCESS if both are same
*
* Parameters
* width - resolution width
*
* Return val
* int - 0 == SUCCESS, -1 = ERROR
*
******************************************************************************/
int is_samestride(unsigned int width)
{
	UINT kmdstride = 0;
	UINT bytes_pp = 0;
	UINT umdstride = 0;
	UINT rem = 0;
	UINT padding = 0;

	bytes_pp = (VGPU_BPP + 7) / 8;
	kmdstride = (width * bytes_pp + 3) & ~0x3;

	rem = width % BYTE_ALIGN_16;
	if (rem != 0) {
		padding = BYTE_ALIGN_16 - rem;
	}
	else {
		padding = 0;
	}
	umdstride = 4 * (width + padding);

	if (kmdstride == umdstride) {
		return DVSERVERUMD_SUCCESS;
	}
	else {
		return DVSERVERUMD_FAILURE;
	}
}
