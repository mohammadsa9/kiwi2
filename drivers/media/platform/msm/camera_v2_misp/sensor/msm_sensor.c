/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "msm_sensor.h"
#include "msm_sd.h"
#include "camera.h"
#include "msm_cci.h"
#include "msm_camera_io_util.h"
#include "msm_camera_i2c_mux.h"
#include <linux/regulator/rpm-smd-regulator.h>
#include <linux/regulator/consumer.h>

#include "sensor_otp_common_if.h"

#include "msm_camera_dsm.h"

/* optimize camera print mipi packet and frame count log*/
#include "./msm.h"
#include "./csid/msm_csid.h"
#include "camera_agent.h"
#include "mini_isp.h"
#include "sensor_otp_common.h"

/*#define CONFIG_MSMB_CAMERA_DEBUG*/
#undef CDBG
#define CDBG(fmt, args...) pr_debug(fmt, ##args)

static struct v4l2_file_operations msm_sensor_v4l2_subdev_fops;
#ifdef CONFIG_HUAWEI_DSM
#define MSM_SENSOR_BUFFER_SIZE 1024

static char camera_dsm_log_buff[MSM_SENSOR_BUFFER_SIZE] = {0}; 

#endif
void camera_report_dsm_err_msm_sensor(struct msm_sensor_ctrl_t *s_ctrl, int type, int err_num , const char* str);
extern bool huawei_cam_is_factory_mode(void);
static void msm_sensor_adjust_mclk(struct msm_camera_power_ctrl_t *ctrl)
{
	int idx;
	struct msm_sensor_power_setting *power_setting;
	for (idx = 0; idx < ctrl->power_setting_size; idx++) {
		power_setting = &ctrl->power_setting[idx];
		if (power_setting->seq_type == SENSOR_CLK &&
			power_setting->seq_val ==  SENSOR_CAM_MCLK) {
			if (power_setting->config_val == 24000000) {
				power_setting->config_val = 23880000;
				CDBG("%s MCLK request adjusted to 23.88MHz\n"
							, __func__);
			}
			break;
		}
	}

	return;
}

static int32_t msm_camera_get_power_settimgs_from_sensor_lib(
	struct msm_camera_power_ctrl_t *power_info,
	struct msm_sensor_power_setting_array *power_setting_array)
{
	int32_t rc = 0;
	uint32_t size;
	struct msm_sensor_power_setting *ps;
	bool need_reverse = 0;

	if ((NULL == power_info->power_setting) ||
		(0 == power_info->power_setting_size)) {

		ps = power_setting_array->power_setting;
		size = power_setting_array->size;
		if ((NULL == ps) || (0 == size)) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -EINVAL;
			goto FAILED_1;
		}

		power_info->power_setting =
		kzalloc(sizeof(*ps) * size, GFP_KERNEL);
		if (!power_info->power_setting) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			goto FAILED_1;
		}
		memcpy(power_info->power_setting,
			power_setting_array->power_setting,
			sizeof(*ps) * size);
		power_info->power_setting_size = size;
	}

	ps = power_setting_array->power_down_setting;
	size = power_setting_array->size_down;
	if (NULL == ps || 0 == size) {
		ps = power_info->power_setting;
		size = power_info->power_setting_size;
		need_reverse = 1;
	}

	power_info->power_down_setting =
	kzalloc(sizeof(*ps) * size, GFP_KERNEL);
	if (!power_info->power_down_setting) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_UP;
	}
	memcpy(power_info->power_down_setting,
		ps,
		sizeof(*ps) * size);
	power_info->power_down_setting_size = size;

	if (need_reverse) {
		int c, end = size - 1;
		struct msm_sensor_power_setting power_down_setting_t;
		for (c = 0; c < size/2; c++) {
			power_down_setting_t =
				power_info->power_down_setting[c];
			power_info->power_down_setting[c] =
				power_info->power_down_setting[end];
			power_info->power_down_setting[end] =
				power_down_setting_t;
			end--;
		}
	}

	return 0;
FREE_UP:
	kfree(power_info->power_setting);
FAILED_1:
	return rc;
}

static int32_t msm_sensor_get_dt_data(struct device_node *of_node,
	struct msm_sensor_ctrl_t *s_ctrl)
{
	int32_t rc = 0, i = 0, ret = 0;
	struct msm_camera_gpio_conf *gconf = NULL;
	struct msm_camera_sensor_board_info *sensordata = NULL;
	uint16_t *gpio_array = NULL;
	uint16_t gpio_array_size = 0;
	uint32_t id_info[3];

	s_ctrl->sensordata = kzalloc(sizeof(
		struct msm_camera_sensor_board_info),
		GFP_KERNEL);
	if (!s_ctrl->sensordata) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		return -ENOMEM;
	}

	sensordata = s_ctrl->sensordata;

	rc = of_property_read_string(of_node, "qcom,sensor-name",
		&sensordata->sensor_name);
	CDBG("%s qcom,sensor-name %s, rc %d\n", __func__,
		sensordata->sensor_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_SENSORDATA;
	}

	rc = of_property_read_u32(of_node, "qcom,cci-master",
		&s_ctrl->cci_i2c_master);
	CDBG("%s qcom,cci-master %d, rc %d\n", __func__, s_ctrl->cci_i2c_master,
		rc);
	if (rc < 0) {
		/* Set default master 0 */
		s_ctrl->cci_i2c_master = MASTER_0;
		rc = 0;
	}

	rc = msm_sensor_get_sub_module_index(of_node, &sensordata->sensor_info);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_SENSORDATA;
	}

	/* Get sensor mount angle */
	if (0 > of_property_read_u32(of_node, "qcom,mount-angle",
		&sensordata->sensor_info->sensor_mount_angle)) {
		/* Invalidate mount angle flag */
		CDBG("%s:%d Default sensor mount angle\n",
			__func__, __LINE__);
		sensordata->sensor_info->is_mount_angle_valid = 0;
		sensordata->sensor_info->sensor_mount_angle = 0;
	} else {
		sensordata->sensor_info->is_mount_angle_valid = 1;
	}
	CDBG("%s qcom,mount-angle %d\n", __func__,
		sensordata->sensor_info->sensor_mount_angle);
	if (0 > of_property_read_u32(of_node, "qcom,sensor-position",
		&sensordata->sensor_info->position)) {
		CDBG("%s:%d Default sensor position\n", __func__, __LINE__);
		sensordata->sensor_info->position = 0;
	}
	CDBG("%s qcom,sensor-position %d\n", __func__,
		sensordata->sensor_info->position);
	if (0 > of_property_read_u32(of_node, "qcom,sensor-mode",
		&sensordata->sensor_info->modes_supported)) {
		CDBG("%s:%d Default sensor mode\n", __func__, __LINE__);
		sensordata->sensor_info->modes_supported = 0;
	}
	CDBG("%s qcom,sensor-mode %d\n", __func__,
		sensordata->sensor_info->modes_supported);

	s_ctrl->set_mclk_23880000 = of_property_read_bool(of_node,
						"qcom,mclk-23880000");

	CDBG("%s qcom,mclk-23880000 %d\n", __func__,
		s_ctrl->set_mclk_23880000);

	rc = msm_sensor_get_dt_csi_data(of_node, &sensordata->csi_lane_params);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_SENSOR_INFO;
	}

	rc = msm_camera_get_dt_vreg_data(of_node,
			&sensordata->power_info.cam_vreg,
			&sensordata->power_info.num_vreg);
	if (rc < 0)
		goto FREE_CSI;

	rc = msm_camera_get_dt_power_setting_data(of_node,
			sensordata->power_info.cam_vreg,
			sensordata->power_info.num_vreg,
			&sensordata->power_info);


	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_VREG;
	}


	rc = msm_camera_get_power_settimgs_from_sensor_lib(
			&sensordata->power_info,
			&s_ctrl->power_setting_array);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_VREG;
	}

	sensordata->power_info.gpio_conf = kzalloc(
			sizeof(struct msm_camera_gpio_conf), GFP_KERNEL);
	if (!sensordata->power_info.gpio_conf) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto FREE_PS;
	}
	gconf = sensordata->power_info.gpio_conf;

	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size) {
		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto FREE_GPIO_CONF;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_camera_get_dt_gpio_req_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto FREE_GPIO_CONF;
		}

		rc = msm_camera_get_dt_gpio_set_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto FREE_GPIO_REQ_TBL;
		}

		rc = msm_camera_init_gpio_pin_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto FREE_GPIO_SET_TBL;
		}
	}
	rc = msm_sensor_get_dt_actuator_data(of_node,
					     &sensordata->actuator_info);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_GPIO_PIN_TBL;
	}

	sensordata->slave_info = kzalloc(sizeof(struct msm_camera_slave_info),
		GFP_KERNEL);
	if (!sensordata->slave_info) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto FREE_ACTUATOR_INFO;
	}

	rc = of_property_read_u32_array(of_node, "qcom,slave-id",
		id_info, 3);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto FREE_SLAVE_INFO;
	}

	sensordata->slave_info->sensor_slave_addr = id_info[0];
	sensordata->slave_info->sensor_id_reg_addr = id_info[1];
	sensordata->slave_info->sensor_id = id_info[2];
	CDBG("%s:%d slave addr 0x%x sensor reg 0x%x id 0x%x\n",
		__func__, __LINE__,
		sensordata->slave_info->sensor_slave_addr,
		sensordata->slave_info->sensor_id_reg_addr,
		sensordata->slave_info->sensor_id);

	/*Optional property, don't return error if absent */
	ret = of_property_read_string(of_node, "qcom,vdd-cx-name",
		&sensordata->misc_regulator);
	CDBG("%s qcom,misc_regulator %s, rc %d\n", __func__,
		 sensordata->misc_regulator, ret);

	kfree(gpio_array);

	return rc;

FREE_SLAVE_INFO:
	kfree(s_ctrl->sensordata->slave_info);
FREE_ACTUATOR_INFO:
	kfree(s_ctrl->sensordata->actuator_info);
FREE_GPIO_PIN_TBL:
	kfree(s_ctrl->sensordata->power_info.gpio_conf->gpio_num_info);
FREE_GPIO_SET_TBL:
	kfree(s_ctrl->sensordata->power_info.gpio_conf->cam_gpio_set_tbl);
FREE_GPIO_REQ_TBL:
	kfree(s_ctrl->sensordata->power_info.gpio_conf->cam_gpio_req_tbl);
FREE_GPIO_CONF:
	kfree(s_ctrl->sensordata->power_info.gpio_conf);
FREE_PS:
	kfree(s_ctrl->sensordata->power_info.power_setting);
	kfree(s_ctrl->sensordata->power_info.power_down_setting);
FREE_VREG:
	kfree(s_ctrl->sensordata->power_info.cam_vreg);
FREE_CSI:
	kfree(s_ctrl->sensordata->csi_lane_params);
FREE_SENSOR_INFO:
	kfree(s_ctrl->sensordata->sensor_info);
FREE_SENSORDATA:
	kfree(s_ctrl->sensordata);
	kfree(gpio_array);
	return rc;
}

static void msm_sensor_misc_regulator(
	struct msm_sensor_ctrl_t *sctrl, uint32_t enable)
{
	int32_t rc = 0;
	if (enable) {
		sctrl->misc_regulator = (void *)rpm_regulator_get(
			&sctrl->pdev->dev, sctrl->sensordata->misc_regulator);
		if (sctrl->misc_regulator) {
			rc = rpm_regulator_set_mode(sctrl->misc_regulator,
				RPM_REGULATOR_MODE_HPM);
			if (rc < 0) {
				pr_err("%s: Failed to set for rpm regulator on %s: %d\n",
					__func__,
					sctrl->sensordata->misc_regulator, rc);
				rpm_regulator_put(sctrl->misc_regulator);
			}
		} else {
			pr_err("%s: Failed to vote for rpm regulator on %s: %d\n",
				__func__,
				sctrl->sensordata->misc_regulator, rc);
		}
	} else {
		if (sctrl->misc_regulator) {
			rc = rpm_regulator_set_mode(
				(struct rpm_regulator *)sctrl->misc_regulator,
				RPM_REGULATOR_MODE_AUTO);
			if (rc < 0)
				pr_err("%s: Failed to set for rpm regulator on %s: %d\n",
					__func__,
					sctrl->sensordata->misc_regulator, rc);
			rpm_regulator_put(sctrl->misc_regulator);
		}
	}
}

int32_t msm_sensor_free_sensor_data(struct msm_sensor_ctrl_t *s_ctrl)
{
	if (!s_ctrl->pdev && !s_ctrl->sensor_i2c_client->client)
		return 0;
	kfree(s_ctrl->sensordata->slave_info);
	kfree(s_ctrl->sensordata->cam_slave_info);
	kfree(s_ctrl->sensordata->actuator_info);
	kfree(s_ctrl->sensordata->power_info.gpio_conf->gpio_num_info);
	kfree(s_ctrl->sensordata->power_info.gpio_conf->cam_gpio_set_tbl);
	kfree(s_ctrl->sensordata->power_info.gpio_conf->cam_gpio_req_tbl);
	kfree(s_ctrl->sensordata->power_info.gpio_conf);
	kfree(s_ctrl->sensordata->power_info.cam_vreg);
	kfree(s_ctrl->sensordata->power_info.power_setting);
	kfree(s_ctrl->sensordata->power_info.power_down_setting);
	kfree(s_ctrl->sensordata->csi_lane_params);
	kfree(s_ctrl->sensordata->sensor_info);
	kfree(s_ctrl->sensordata->power_info.clk_info);
	kfree(s_ctrl->sensordata);
	return 0;
}

static struct msm_cam_clk_info cam_8960_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_clk", 24000000},
};

static struct msm_cam_clk_info cam_8610_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_src_clk", 24000000},
	[SENSOR_CAM_CLK] = {"cam_clk", 0},
};
static struct msm_cam_clk_info cam_8974_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_src_clk", 24000000},
	[SENSOR_CAM_CLK] = {"cam_clk", 0},
	[SENSOR_MINIISP_MCLK] = {"isp_src_clk", 24000000},
	[SENSOR_MINIISP_CLK] = {"isp_clk", 0},
};
static int msm_sensor_check_mcam_id(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc=0,mcam_id=-1;
	const char *sensor_name;
	unsigned gpio;
	if(!s_ctrl->sensordata->power_info.gpio_conf->gpio_num_info->valid[SENSOR_GPIO_CAM_ID])
		return 0;
	gpio=s_ctrl->sensordata->power_info.gpio_conf->gpio_num_info->gpio_num[SENSOR_GPIO_CAM_ID];
	sensor_name = s_ctrl->sensordata->sensor_name;
	if(s_ctrl->sensordata->slave_info->mcam_id==1 || s_ctrl->sensordata->slave_info->mcam_id==0){
		mcam_id=gpio_get_value(gpio);
		if(mcam_id==s_ctrl->sensordata->slave_info->mcam_id){
			CDBG("%s:%s gpio %d except value:%d match",__func__,sensor_name,gpio,mcam_id);
			rc=0;
		}else{
			pr_err("%s:%s gpio %d value:%d not match",__func__,sensor_name,gpio,mcam_id);
			rc=-1;
		}
       }else{
               CDBG("%s:%s gpio %d value:%d no need to match CAMID",__func__,sensor_name,gpio,mcam_id);
               return 0;
	}
	return rc;
}
#if 0
static int msm_sensor_check_module_vendor_with_otp(struct msm_sensor_ctrl_t *s_ctrl, int *read_otp)
{
	int rc = -EINVAL, pos = 0;

	if (!s_ctrl) {
		pr_err("%s:%d failed: %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}
	if(BACK_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		pos = 0;
	else if(FRONT_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		pos = 1;
	else
		pr_err("wrong sensor position");

	misp_get_otp_data(s_ctrl,read_otp, pos, 1252);
	if (*read_otp != 0)
	{
		if ((*read_otp >> 4) == s_ctrl->sensordata->slave_info->otp_vendor_id)
		{
			rc = 0;
			pr_debug("%s. get otp id ok\n",__func__);
		}
		else
		{
			pr_err("%s. get otp vendor id not match. expetct: %x, read: %x\n", __func__, 
						s_ctrl->sensordata->slave_info->otp_vendor_id, *read_otp);
		}
	}
	return rc;
}
#endif
int msm_sensor_get_otp(struct msm_sensor_ctrl_t *s_ctrl,struct msm_sensor_otp_info *otp_info)
{
	int rc = -EINVAL;
	int i = 0;
	struct msm_camera_spi_reg_settings spi_regs;
	struct msm_camera_spi_array spi_array[2] = {{0},{0}};
	uint8_t param[9];
	uint8_t *otp_data = NULL;
	uint8_t *paddr=(uint8_t*)&param[0];
	uint32_t *ptotal=(uint32_t*)&param[1];
	uint32_t *pblock=(uint32_t*)&param[5];
	uint32_t otp_length = otp_info->otp_size;
	if (!s_ctrl) {
		pr_err("%s:%d failed: %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}
	if(!(otp_info && otp_info->otp_size)){
		pr_err("%s:%d faild: otp_info",__func__,__LINE__);
		return -EINVAL;
	}
	otp_data = kzalloc(otp_info->otp_size, GFP_KERNEL);
	if(!otp_data){
		pr_err("%s failed: no memory for otp_data\n", __func__);
		return -ENOMEM;
	}

	//get otp command
	spi_array[0].opcode = ISPCMD_BULK_GET_OTP_DATA;
	spi_array[0].size = sizeof(param);
	spi_array[0].is_wait_state = true;
	spi_array[0].wait_state = MINI_ISP_STATE_READY;
	spi_array[0].param = param;
	if(BACK_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		*paddr = 0;
	else if(FRONT_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		*paddr = 1;
	else{
		pr_err("wrong sensor position");
		goto out;
	}
	*ptotal = otp_info->otp_size;
	*pblock = 4096;//ask altek advice should be below 8192
	//recv data command
	spi_array[1].size = otp_info->otp_size;
	spi_array[1].is_block_data = true;
	spi_array[1].is_recv = true;
	spi_array[1].param = otp_data;

	spi_regs.size = sizeof(spi_array)/sizeof(struct msm_camera_spi_array);
	spi_regs.reg_settings = spi_array;
	rc = misp_execmd_array(spi_regs);
	if(rc){
		pr_err("%s %d: misp_execmd faild",__func__,__LINE__);
		goto out;
	}
	// handle otp data
	pr_info("%s: otp data: DATE=20%d.%d.%d, huawei ID=%d, module ID=%d \n",
		__func__, otp_data[0], otp_data[1], otp_data[2], otp_data[3], otp_data[4]);
	pr_info("%s, is otp data vaild=%d \n", __func__, otp_data[otp_info->otp_size-1]);

	if(otp_info->ois_otp.ois_size)
	{
		otp_info->otp_vaild = otp_data[otp_length - 2];//in ois case ucDataVaild is otp_length-2
	} else {
		otp_info->otp_vaild = otp_data[otp_length-1];
	}
	if(otp_info->otp_vaild){
		//copy common info
		if(otp_info->common_otp.common_size)
		{
			memcpy(otp_info->common_otp.common_info,otp_data,otp_info->common_otp.common_size);
			pr_info("common otp info[%d]:\n",otp_info->common_otp.common_size);
			for(i = 0; i<otp_info->common_otp.common_size; i++)
			{
				pr_info("0x%02x ",otp_info->common_otp.common_info[i]);
			}
			pr_info("\n");
		}
		//copy awb info
		if(otp_info->awb_otp.awb_size)
		{
			memcpy(otp_info->awb_otp.aucISO_AWBCalib,&otp_data[otp_info->awb_otp.index_start],
				otp_info->awb_otp.awb_size);
			pr_info("awb otp info[%d]:\n",otp_info->awb_otp.awb_size);
			for(i = 0; i<otp_info->awb_otp.awb_size; i++)
			{
				pr_info("0x%02x ",otp_info->awb_otp.aucISO_AWBCalib[i]);
			}
			pr_info("\n");
		}
		//copy af info
		if(otp_info->af_otp.af_size){
			otp_info->af_otp.start_code =
			otp_data[otp_info->af_otp.index_start] << 8
				| otp_data[otp_info->af_otp.index_start +1];
			otp_info->af_otp.max_code =
			otp_data[otp_info->af_otp.index_start + 2] << 8
				| otp_data[otp_info->af_otp.index_start + 3];
			pr_err("vcm current:  start=%d, max=%d",
				otp_info->af_otp.start_code,
				otp_info->af_otp.max_code);
		}
		//copy ois info
		if(otp_info->ois_otp.ois_size)
		{
			memcpy(otp_info->ois_otp.aucOIS,&otp_data[otp_info->ois_otp.index_start],
				otp_info->ois_otp.ois_size);
			pr_info("ois otp info[%d]:\n",otp_info->ois_otp.ois_size);
			for(i = 0; i<otp_info->ois_otp.ois_size; i++)
			{
				pr_info("0x%02x ",otp_info->ois_otp.aucOIS[i]);
			}
			pr_info("ucDataValid =0x%02x \n", otp_data[otp_length - 2]);
			pr_info("ucHallLimitDataValid = 0x%02x \n",otp_data[otp_length - 1]);
		}
	} else {
		pr_err("%s, otp data not vaild \n", __func__);
		rc = -EINVAL;
		goto out;
	}
out:
	kfree(otp_data);
	return rc;
}

int msm_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	struct msm_camera_power_ctrl_t *power_info;
	enum msm_camera_device_type_t sensor_device_type;
	struct msm_camera_i2c_client *sensor_i2c_client;

	if (!s_ctrl) {
		pr_err("%s:%d failed: s_ctrl %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}

	power_info = &s_ctrl->sensordata->power_info;
	sensor_device_type = s_ctrl->sensor_device_type;
	sensor_i2c_client = s_ctrl->sensor_i2c_client;

	if (!power_info || !sensor_i2c_client) {
		pr_err("%s:%d failed: power_info %p sensor_i2c_client %p\n",
			__func__, __LINE__, power_info, sensor_i2c_client);
		return -EINVAL;
	}
	return msm_camera_power_down(power_info, sensor_device_type,
		sensor_i2c_client);
}

int msm_sensor_match_id_misp(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	uint16_t chipid =0;
	u8 param[3]={0};
	struct msm_camera_slave_info *slave_info;
	const char *sensor_name;
	struct misp_askdata_setting setting={
		.askparam=NULL,
		.asklen=0,
		.wait_state=MINI_ISP_STATE_READY,
		.is_block_data=0,
	};
	if (!s_ctrl) {
		pr_err("%s:%d failed: %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}
	slave_info = s_ctrl->sensordata->slave_info;
	sensor_name = s_ctrl->sensordata->sensor_name;
	setting.recvparam = param;
	setting.recvlen = sizeof(param);
	if(BACK_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		setting.opcode = ISPCMD_SYSTEM_GET_FIRSTSENSORID;
	else if(FRONT_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		setting.opcode = ISPCMD_SYSTEM_GET_SECONDSENSORID;
	else{
		pr_err("wrong sensor position");
	}
	if (!slave_info || !sensor_name) {
		pr_err("%s:%d failed: %p %p\n",
			__func__, __LINE__, slave_info,
			sensor_name);
		return -EINVAL;
	}
	/*get sensor id through miniisp*/
	rc = misp_load_fw(s_ctrl);
	if (rc < 0) {
		pr_err("%s: %s: load firmware failed\n", __func__, sensor_name);
		return rc;
	}
	rc =  misp_askdata_cmd(setting);
	pr_debug("%s. param[0]=%x [1]=%x [2]=%x\n",__func__,param[0],param[1],param[2]);
	if (rc < 0) {
		pr_err("%s: %s: read id failed\n", __func__, sensor_name);
		return rc;
	}
	chipid = param[0]+(param[1]<<8);;
	pr_debug("%s: read id: 0x%x expected id 0x%x:\n", __func__, chipid,
		slave_info->sensor_id);
	if (chipid != slave_info->sensor_id) {
		pr_err("msm_sensor_match_id chip id doesnot match\n");
		pr_err("%s: read id: 0x%x expected id 0x%x:\n",__func__,chipid,slave_info->sensor_id);
		return -ENODEV;
	}
	return rc;
}

int msm_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc;
	struct msm_camera_power_ctrl_t *power_info;
	struct msm_camera_i2c_client *sensor_i2c_client;
	struct msm_camera_slave_info *slave_info;
	const char *sensor_name;
	uint32_t retry = 0;
	int8_t otp_index = -1;
	int pos = 0;
	int8_t vendor_id = 0;
	int read_otp = 0;

	if (!s_ctrl) {
		pr_err("%s:%d failed: %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}

	power_info = &s_ctrl->sensordata->power_info;
	sensor_i2c_client = s_ctrl->sensor_i2c_client;
	slave_info = s_ctrl->sensordata->slave_info;
	sensor_name = s_ctrl->sensordata->sensor_name;

	if (!power_info || !sensor_i2c_client || !slave_info ||
		!sensor_name) {
		pr_err("%s:%d failed: %p %p %p %p\n",
			__func__, __LINE__, power_info,
			sensor_i2c_client, slave_info, sensor_name);
		return -EINVAL;
	}

	if (s_ctrl->set_mclk_23880000)
		msm_sensor_adjust_mclk(power_info);
    
	if(BACK_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		pos = 0;
	else if(FRONT_CAMERA_B == s_ctrl->sensordata->sensor_info->position)
		pos = 1;
	else
		pr_err("wrong sensor position");

	otp_index = get_otp_index(s_ctrl);
	if(otp_index < 0)
	{
		pr_err("%s: get_otp_index %d fail \n",__func__,otp_index );
		return -EINVAL;
	}
	for (retry = 0; retry < 3; retry++) {
		rc = msm_camera_power_up(power_info, s_ctrl->sensor_device_type,
			sensor_i2c_client);
		if (rc < 0)
			return rc;

		rc = msm_sensor_check_id(s_ctrl);
		if (rc < 0) {
			msm_camera_power_down(power_info,
				s_ctrl->sensor_device_type, sensor_i2c_client);
			//from msleep(20) to mdelay(2)
			mdelay(2);
			continue;
		}
		else
		{
			pr_info("%s: %s check chip id success \n",__func__,s_ctrl->sensordata->sensor_name);
			if(!read_otp)
			{
				rc = msm_sensor_get_otp(s_ctrl,otp_data_lists[otp_index].otp_info);
				if(rc < 0 )
				{
					pr_err("%s: msm_sensor_get_otp() failed for %s, rc=%d, retry=%d\n",
							__func__, s_ctrl->sensordata->sensor_name, rc, retry);
					// TODO here we can hold up the sensor with no otp!
					if(huawei_cam_is_factory_mode()){
						pr_err("%s: we are in factory mode, to power up and try to get otp again!\n", __func__);
						msm_camera_power_down(power_info,
							s_ctrl->sensor_device_type, sensor_i2c_client);
						continue;
					}
				}
				else
				{
					read_otp = 1;
					//read ucVenderAndVersion
					vendor_id = otp_data_lists[otp_index].otp_info->common_otp.common_info[4];
				}
			}

		}

		if (0 == s_ctrl->sensordata->slave_info->otp_vendor_id)
		{
			pr_info("%s no need to check otp for vendor id\n",__func__);
			rc = msm_sensor_check_mcam_id(s_ctrl);
			if(rc < 0)
			{
				msm_camera_power_down(power_info,
					s_ctrl->sensor_device_type, sensor_i2c_client);
				continue;
			}
			else
			{
				pr_info("%s. check mcam id ok\n",__func__);
				break;
			}
		}
		else
		{
			//rc = msm_sensor_check_module_vendor_with_otp(s_ctrl, &vendor_id);
			if((vendor_id >> 4) == s_ctrl->sensordata->slave_info->otp_vendor_id)
			{
				rc = 0;
				pr_info("%s. check vendor id ok\n",__func__);
				break;
			}
			else
			{
				read_otp = 0;
				pr_err("%s.vendor id not match except:%d current:%d\n",__func__,s_ctrl->sensordata->slave_info->otp_vendor_id,(vendor_id >> 4));
				msm_camera_power_down(power_info,
					s_ctrl->sensor_device_type, sensor_i2c_client);
				rc = -EINVAL;
				break;
			}
		}
	}

	return rc;
}
int msm_sensor_match_id(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	uint16_t chipid = 0;
	struct msm_camera_i2c_client *sensor_i2c_client;
	struct msm_camera_slave_info *slave_info;
	const char *sensor_name;

	if (!s_ctrl) {
		pr_err("%s:%d failed: %p\n",
			__func__, __LINE__, s_ctrl);
		return -EINVAL;
	}
	sensor_i2c_client = s_ctrl->sensor_i2c_client;
	slave_info = s_ctrl->sensordata->slave_info;
	sensor_name = s_ctrl->sensordata->sensor_name;

	if (!sensor_i2c_client || !slave_info || !sensor_name) {
		pr_err("%s:%d failed: %p %p %p\n",
			__func__, __LINE__, sensor_i2c_client, slave_info,
			sensor_name);
		return -EINVAL;
	}

	rc = sensor_i2c_client->i2c_func_tbl->i2c_read(
		sensor_i2c_client, slave_info->sensor_id_reg_addr,
		&chipid, MSM_CAMERA_I2C_WORD_DATA);
	if (rc < 0) {
		pr_err("%s: %s: read id failed\n", __func__, sensor_name);
		return rc;
	}

	CDBG("%s: read id: 0x%x expected id 0x%x:\n", __func__, chipid,
		slave_info->sensor_id);
	if (chipid != slave_info->sensor_id) {
		pr_err("msm_sensor_match_id chip id doesnot match\n");
		return -ENODEV;
	}
	return rc;
}

static struct msm_sensor_ctrl_t *get_sctrl(struct v4l2_subdev *sd)
{
	return container_of(container_of(sd, struct msm_sd_subdev, sd),
		struct msm_sensor_ctrl_t, msm_sd);
}

static void msm_sensor_stop_stream(struct msm_sensor_ctrl_t *s_ctrl)
{
	mutex_lock(s_ctrl->msm_sensor_mutex);
	if (s_ctrl->sensor_state == MSM_SENSOR_POWER_UP) {
		s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write_table(
			s_ctrl->sensor_i2c_client, &s_ctrl->stop_setting);
		kfree(s_ctrl->stop_setting.reg_setting);
		s_ctrl->stop_setting.reg_setting = NULL;
	}
	mutex_unlock(s_ctrl->msm_sensor_mutex);
	return;
}

static int msm_sensor_get_af_status(struct msm_sensor_ctrl_t *s_ctrl,
			void __user *argp)
{
	/* TO-DO: Need to set AF status register address and expected value
	We need to check the AF status in the sensor register and
	set the status in the *status variable accordingly*/
	return 0;
}

static long msm_sensor_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	int rc = 0;
	struct msm_sensor_ctrl_t *s_ctrl = get_sctrl(sd);
	void __user *argp = (void __user *)arg;
	if (!s_ctrl) {
		pr_err("%s s_ctrl NULL\n", __func__);
		return -EBADF;
	}
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_CFG:
#ifdef CONFIG_COMPAT
		if (is_compat_task())
			rc = s_ctrl->func_tbl->sensor_config32(s_ctrl, argp);
		else
#endif
			rc = s_ctrl->func_tbl->sensor_config(s_ctrl, argp);
		return rc;
	case VIDIOC_MSM_SENSOR_GET_AF_STATUS:
		return msm_sensor_get_af_status(s_ctrl, argp);
	case VIDIOC_MSM_SENSOR_RELEASE:
	case MSM_SD_SHUTDOWN:
		msm_sensor_stop_stream(s_ctrl);
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}
/* optimize camera print mipi packet and frame count log*/
static int read_times = 0;
#define HW_PRINT_PACKET_NUM_TIME 5 //print 5 times
#define HW_READ_PACKET_NUM_TIME 100 //100ms
#define MAX_CSID_NUM 2
struct hw_sensor_fct {
    char *sensor_name; //sensor name from sensor_init.c
    uint32_t fct_reg_addr; //the frame count reg addr
    enum msm_camera_i2c_data_type type; //frame count reg data type
};

/*
G760S use follow sensors:
sensor name      frame count reg addr
imx214_sunny       ---0x0005
imx214_foxconn     ---0x0005
s5k4e1_sunny       ---0x0005
ov5648_foxconn     ---no frame count reg
*/
struct hw_sensor_fct sensor_fct_list[] ={
    {"imx214_sunny",0x0005,MSM_CAMERA_I2C_BYTE_DATA},
    {"imx214_foxconn",0x0005,MSM_CAMERA_I2C_BYTE_DATA},
    {"s5k4e1_sunny",0x0005,MSM_CAMERA_I2C_BYTE_DATA},
};

static int hw_sensor_read_framecount(struct msm_sensor_ctrl_t *s_ctrl)
{
    int rc = -1;
    int i = 0;
    //uint32_t fct_reg_default = 0x0005;
    uint16_t framecount = 0;
    struct msm_camera_i2c_client *sensor_i2c_client = NULL;
    int sensor_list_size = sizeof(sensor_fct_list) / sizeof(sensor_fct_list[0]);
    
    if(!s_ctrl || !s_ctrl->sensordata || !s_ctrl->sensordata->sensor_name)
        return rc;

    //check current sensor support read frame count reg?
    for(i  = 0; i < sensor_list_size; i++)
    {
        if(0 == strcmp(s_ctrl->sensordata->sensor_name, sensor_fct_list[i].sensor_name))
        {
            break;
        }
    }
    if(i >= sensor_list_size)
    {
        pr_info("%s:%s don't support read frame count \n",__func__,
            s_ctrl->sensordata->sensor_name);
        return rc;
    }
    
    //pr_info("%s:%s i=%d support read frame count! \n",__func__,
                //s_ctrl->sensordata->sensor_name, i);

    sensor_i2c_client = s_ctrl->sensor_i2c_client;

    if(!sensor_i2c_client)
    {
        pr_err("%s: sensor_i2c_client is NULL \n",__func__);
        return rc;
    }

    rc = sensor_i2c_client->i2c_func_tbl->i2c_read(
		sensor_i2c_client, sensor_fct_list[i].fct_reg_addr,
		&framecount, sensor_fct_list[i].type);

    if(rc < 0)
    {
        pr_err("%s: read framecount failed\n", __func__);
		return rc;
    }

    return framecount;
}

static void read_framecount_work_handler(struct work_struct *work)
{
    struct v4l2_subdev *subdev_csids[MAX_CSID_NUM] = {NULL};
    struct csid_device *csid_dev = NULL;
    
    int i = 0;
	struct msm_sensor_ctrl_t *s_ctrl = container_of(work, struct msm_sensor_ctrl_t,frm_cnt_work.work);
	if(!s_ctrl || s_ctrl->sensor_state != MSM_SENSOR_POWER_UP || (read_times <= 0))
	{
        read_times = 0;
		pr_err("%s:%d\n",__func__,__LINE__);
		return;
	}

    //read sensor send
    if(s_ctrl->func_tbl->sensor_read_framecount)
    {
        int frm_cnt = s_ctrl->func_tbl->sensor_read_framecount(s_ctrl);
        if(frm_cnt > 0)
            pr_info("%s: read_times[%d] framecount = %d \n",__func__,read_times,frm_cnt);
    }

    //read csid receive 
    msm_sd_get_subdevs(subdev_csids,MAX_CSID_NUM,"msm_csid");
    
    //we have two csids
    //uint32_t subdev_id[MAX_CSID_NUM] = {0};
    for(i = 0; i<MAX_CSID_NUM; i++)
    {
        if(!subdev_csids[i])
            continue;
        /*
        v4l2_subdev_call(subdev_csids[i], core, ioctl, 
            VIDIOC_MSM_SENSOR_GET_SUBDEV_ID, &subdev_id[i]);
        */
        if(subdev_csids[i]->dev_priv)
            csid_dev = subdev_csids[i]->dev_priv;

        if(!csid_dev)
        {
            read_times = 0;
            pr_err("%s: csid_dev[%d] is NULL \n",__func__,i);
            continue;
        }

        //back camera use csid0, front camera use csid1
        if(csid_dev->csid_state == CSID_POWER_UP && csid_dev->csid_read_mipi_pkg)
        {
            uint32_t csid_pkg = csid_dev->csid_read_mipi_pkg(csid_dev);
            pr_info("%s: csid[%d] read_times[%d] total mipi packet = %u\n",__func__,
                csid_dev->pdev->id, read_times,csid_pkg);
            break;
        }
    }

    read_times--;
	if(read_times > 0) {
		  schedule_delayed_work(&s_ctrl->frm_cnt_work, msecs_to_jiffies(HW_READ_PACKET_NUM_TIME));
	}
    
}

#ifdef CONFIG_COMPAT
static long msm_sensor_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_CFG32:
		cmd = VIDIOC_MSM_SENSOR_CFG;
	default:
		return msm_sensor_subdev_ioctl(sd, cmd, arg);
	}
}

long msm_sensor_subdev_fops_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_sensor_subdev_do_ioctl);
}

static int msm_sensor_config32(struct msm_sensor_ctrl_t *s_ctrl,
	void __user *argp)
{
	struct sensorb_cfg_data32 *cdata = (struct sensorb_cfg_data32 *)argp;
	int32_t rc = 0;
	int32_t i = 0;

	int32_t index = -1;
	//delete

	mutex_lock(s_ctrl->msm_sensor_mutex);
	CDBG("%s:%d %s cfgtype = %d\n", __func__, __LINE__,
		s_ctrl->sensordata->sensor_name, cdata->cfgtype);
	switch (cdata->cfgtype) {
	case CFG_GET_SENSOR_INFO:
		memcpy(cdata->cfg.sensor_info.sensor_name,
			s_ctrl->sensordata->sensor_name,
			sizeof(cdata->cfg.sensor_info.sensor_name));
		cdata->cfg.sensor_info.session_id =
			s_ctrl->sensordata->sensor_info->session_id;
		for (i = 0; i < SUB_MODULE_MAX; i++) {
			cdata->cfg.sensor_info.subdev_id[i] =
				s_ctrl->sensordata->sensor_info->subdev_id[i];
			cdata->cfg.sensor_info.subdev_intf[i] =
				s_ctrl->sensordata->sensor_info->subdev_intf[i];
		}
		cdata->cfg.sensor_info.is_mount_angle_valid =
			s_ctrl->sensordata->sensor_info->is_mount_angle_valid;
		cdata->cfg.sensor_info.sensor_mount_angle =
			s_ctrl->sensordata->sensor_info->sensor_mount_angle;
		cdata->cfg.sensor_info.position =
			s_ctrl->sensordata->sensor_info->position;
		cdata->cfg.sensor_info.modes_supported =
			s_ctrl->sensordata->sensor_info->modes_supported;
		CDBG("%s:%d sensor name %s\n", __func__, __LINE__,
			cdata->cfg.sensor_info.sensor_name);
		CDBG("%s:%d session id %d\n", __func__, __LINE__,
			cdata->cfg.sensor_info.session_id);
		for (i = 0; i < SUB_MODULE_MAX; i++) {
			CDBG("%s:%d subdev_id[%d] %d\n", __func__, __LINE__, i,
				cdata->cfg.sensor_info.subdev_id[i]);
			CDBG("%s:%d subdev_intf[%d] %d\n", __func__, __LINE__,
				i, cdata->cfg.sensor_info.subdev_intf[i]);
		}
		CDBG("%s:%d mount angle valid %d value %d\n", __func__,
			__LINE__, cdata->cfg.sensor_info.is_mount_angle_valid,
			cdata->cfg.sensor_info.sensor_mount_angle);

		break;

	case CFG_GET_OTP: {
		int8_t otp_index;
		otp_index = get_otp_index(s_ctrl);
		if(otp_index >= 0)
		{
			cdata->cfg.otp_info.otp_vaild = 
				otp_data_lists[otp_index].otp_info->otp_vaild;
			cdata->cfg.otp_info.af_otp.start_code = 
				otp_data_lists[otp_index].otp_info->af_otp.start_code;
			cdata->cfg.otp_info.af_otp.max_code = 
				otp_data_lists[otp_index].otp_info->af_otp.max_code;
			cdata->cfg.otp_info.af_otp.start_dist = 
				otp_data_lists[otp_index].otp_info->af_otp.start_dist;
			cdata->cfg.otp_info.af_otp.end_dist = 
				otp_data_lists[otp_index].otp_info->af_otp.end_dist;
			cdata->cfg.otp_info.af_otp.axis_angle= 
				otp_data_lists[otp_index].otp_info->af_otp.axis_angle;
			cdata->cfg.otp_info.af_otp.pose_offset= 
				otp_data_lists[otp_index].otp_info->af_otp.pose_offset;

			CDBG("%s, %d, otp vaild=%d, start code=%d, max code=%d start dist=%d end dist=%d axis angle=%d pose offset=%d\n",
				__func__, __LINE__,
				otp_data_lists[otp_index].otp_info->otp_vaild,
				otp_data_lists[otp_index].otp_info->af_otp.start_code,
				otp_data_lists[otp_index].otp_info->af_otp.max_code,
				otp_data_lists[otp_index].otp_info->af_otp.start_dist,
				otp_data_lists[otp_index].otp_info->af_otp.end_dist,
				otp_data_lists[otp_index].otp_info->af_otp.axis_angle,
				otp_data_lists[otp_index].otp_info->af_otp.pose_offset);
		}else
			rc = -EFAULT;

		break;
	}

	//delete LOAD_FIRMWARE MISP_STREAMON/MISP_STREAMOFF
	case CFG_WRITE_SPI_ARRAY: {
		struct msm_camera_spi_reg_setting32 spi_reg32;
		struct msm_camera_spi_reg_setting spi_reg;
		uint8_t* param;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&spi_reg32,
			(void*)compat_ptr(cdata->cfg.setting),
			sizeof(struct msm_camera_spi_reg_setting32))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		spi_reg.size = spi_reg32.size;
		spi_reg.opcode = spi_reg32.opcode;
		spi_reg.delay = spi_reg32.delay;
		spi_reg.param = compat_ptr(spi_reg32.param);

		if (!spi_reg.size) {
			pr_err("%s:%d spi_reg.size = 0\n", __func__, __LINE__);
		} else {
			param = kzalloc(spi_reg.size *sizeof(uint8_t), GFP_KERNEL);
			if (!param) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -ENOMEM;
				break;
			}
			if (copy_from_user(param, (void *)(spi_reg.param),
				spi_reg.size *
				sizeof(uint8_t))) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				kfree(param);
				rc = -EFAULT;
				break;
			}
		}
		spi_reg.param = param;
		// send by spi
		misp_write_cmd(spi_reg.opcode,spi_reg.param,spi_reg.size);
		if (spi_reg.size){
			kfree(param);
		}
		break;
	}
	case CFG_SPI_REG_SETTINGS: {
		int j = 0;
		struct msm_camera_spi_reg_settings32 spi_regs32;
		struct msm_camera_spi_reg_settings spi_regs;
		struct msm_camera_spi_array32* spi_array32_ptr; //work ptr
		struct msm_camera_spi_array32 spi_array32; //work ptr
		struct msm_camera_spi_array* spi_array;
		uint8_t* param;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		if (copy_from_user(&spi_regs32,
			(void*)compat_ptr(cdata->cfg.setting),
			sizeof(struct msm_camera_spi_reg_settings32))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		spi_array32_ptr = (struct msm_camera_spi_array32*)compat_ptr(spi_regs32.reg_settings);
		spi_regs.size = spi_regs32.size;
		if (spi_regs.size <= 0) {
			pr_err("%s:%d spi_regs.size = 0 size can not be 0\n", __func__, __LINE__);
			rc = -EFAULT;
			break;//size can not be 0
		}else {
			spi_array = kzalloc(spi_regs.size *sizeof(struct msm_camera_spi_array), GFP_KERNEL);
			if (!spi_array) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -ENOMEM;
				break;
			}
		}
		for (j = 0;j < spi_regs.size;j++){
			memset(&spi_array32,0,sizeof(spi_array32));
			if (copy_from_user(&spi_array32,
				(void*)compat_ptr(spi_regs32.reg_settings+(j*(sizeof(struct msm_camera_spi_array32)))),
				sizeof(struct msm_camera_spi_array32))) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -EFAULT;
				goto PARAM_ERROR;
			}
			spi_array[j].size = spi_array32.size;
			spi_array[j].opcode = spi_array32.opcode;
			spi_array[j].is_block_data = spi_array32.is_block_data;
			spi_array[j].is_recv = spi_array32.is_recv;
			spi_array[j].is_wait_state = spi_array32.is_wait_state;
			spi_array[j].wait_state = spi_array32.wait_state;

	                pr_err(".size:%d .opcode:%d .is_block_data:%d .is_recv:%d .is_wait_state:%d .wait_state:%d\n",
			spi_array[j].size,
			spi_array[j].opcode,
			spi_array[j].is_block_data,
			spi_array[j].is_recv,
			spi_array[j].is_wait_state,
			spi_array[j].wait_state);
			// get param of spi_array
			if (!spi_array[j].size) {
				pr_err("%s:%d spi_reg.size = 0\n", __func__, __LINE__);
				spi_array[j].param = NULL;
			} else {
				param = kzalloc(spi_array[j].size *sizeof(uint8_t), GFP_KERNEL);
				if (!param) {
					pr_err("%s:%d failed\n", __func__, __LINE__);
					rc = -ENOMEM; //need to release [0-i]'s param
					goto PARAM_ERROR;
				}
				if (copy_from_user(param, (void *)(compat_ptr(spi_array32.param)),
					spi_array[j].size *sizeof(uint8_t))) {
					pr_err("%s:%d failed\n", __func__, __LINE__);
					kfree(param);
					rc = -EFAULT;
					goto PARAM_ERROR;
				}
				spi_array[j].param = param;
			}
		}
		// send by spi
		spi_regs.reg_settings = spi_array;
		rc = misp_execmd_array(spi_regs);
PARAM_ERROR:
		j--;
		for (;j >= 0 ;j--){
			if (spi_array[j].size){
				kfree(spi_array[j].param);
			}
		}
		kfree(spi_array);
		break;
		}
	case CFG_GET_SENSOR_INIT_PARAMS:
		cdata->cfg.sensor_init_params.modes_supported =
			s_ctrl->sensordata->sensor_info->modes_supported;
		cdata->cfg.sensor_init_params.position =
			s_ctrl->sensordata->sensor_info->position;
		cdata->cfg.sensor_init_params.sensor_mount_angle =
			s_ctrl->sensordata->sensor_info->sensor_mount_angle;
		CDBG("%s:%d init params mode %d pos %d mount %d\n", __func__,
			__LINE__,
			cdata->cfg.sensor_init_params.modes_supported,
			cdata->cfg.sensor_init_params.position,
			cdata->cfg.sensor_init_params.sensor_mount_angle);
		break;
	case CFG_WRITE_I2C_ARRAY: {
		struct msm_camera_i2c_reg_setting32 conf_array32;
		struct msm_camera_i2c_reg_setting conf_array;
		struct msm_camera_i2c_reg_array *reg_setting = NULL;

		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&conf_array32,
			(void *)compat_ptr(cdata->cfg.setting),
			sizeof(struct msm_camera_i2c_reg_setting32))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		conf_array.addr_type = conf_array32.addr_type;
		conf_array.data_type = conf_array32.data_type;
		conf_array.delay = conf_array32.delay;
		conf_array.size = conf_array32.size;
		conf_array.reg_setting = compat_ptr(conf_array32.reg_setting);

		if (!conf_array.size) {
			pr_err("%s:%d conf_array.size = 0\n", __func__, __LINE__);
			//rc = -EFAULT;
			break;
		}

		reg_setting = kzalloc(conf_array.size *
			(sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
		if (!reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(reg_setting,
			(void *)(conf_array.reg_setting),
			conf_array.size *
			sizeof(struct msm_camera_i2c_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}

		conf_array.reg_setting = reg_setting;

		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->
			i2c_write_table(s_ctrl->sensor_i2c_client,
			&conf_array);

		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_WRITE_I2C_ARRAY" );
		}

		kfree(reg_setting);
		break;
	}
        case CFG_WRITE_EXPOSURE_DATA: {
            struct msm_camera_i2c_reg_setting32 conf_array32;
            struct msm_camera_i2c_reg_setting conf_array;
            struct msm_camera_i2c_reg_array *reg_setting = NULL;

            if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
                 pr_err("%s:%d failed: invalid state %d\n", __func__,
                     __LINE__, s_ctrl->sensor_state);
                 rc = -EFAULT;
                 break;
            }

            if (copy_from_user(&conf_array32,
                 (void *)compat_ptr(cdata->cfg.setting),
                 sizeof(struct msm_camera_i2c_reg_setting32))) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 rc = -EFAULT;
                 break;
            }

            conf_array.addr_type = conf_array32.addr_type;
            conf_array.data_type = conf_array32.data_type;
            conf_array.delay = conf_array32.delay;
            conf_array.size = conf_array32.size;
            conf_array.reg_setting = compat_ptr(conf_array32.reg_setting);

            if (!conf_array.size) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 break;
            }

            reg_setting = kzalloc(conf_array.size *
                 (sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
            if (!reg_setting) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 rc = -ENOMEM;
                 break;
            }
            if (copy_from_user(reg_setting, (void *)conf_array.reg_setting,
                 conf_array.size *
                 sizeof(struct msm_camera_i2c_reg_array))) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 kfree(reg_setting);
                 rc = -EFAULT;
                 break;
            }

            conf_array.reg_setting = reg_setting;
            for (i = 0; i < conf_array.size; i++) {
                if(conf_array.reg_setting[i].data_type){
                   rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write(
                       s_ctrl->sensor_i2c_client, conf_array.reg_setting[i].reg_addr, conf_array.reg_setting[i].reg_data,
                       conf_array.reg_setting[i].data_type);
                }
                else{
                     rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write(
                         s_ctrl->sensor_i2c_client, conf_array.reg_setting[i].reg_addr, conf_array.reg_setting[i].reg_data,
                         conf_array.data_type);
                }
            }
		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_WRITE_EXPOSURE_DATA");
		}
            if (conf_array.delay > 20)
                msleep(conf_array.delay);
            else if (conf_array.delay)
                usleep_range(conf_array.delay * 1000, (conf_array.delay
                  * 1000) + 1000);

            kfree(reg_setting);
            break;
        }
	case CFG_SLAVE_READ_I2C: {
		struct msm_camera_i2c_read_config read_config;
		uint16_t local_data = 0;
		uint16_t orig_slave_addr = 0, read_slave_addr = 0;
		if (copy_from_user(&read_config,
			(void *)compat_ptr(cdata->cfg.setting),
			sizeof(struct msm_camera_i2c_read_config))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		read_slave_addr = read_config.slave_addr;
		CDBG("%s:CFG_SLAVE_READ_I2C:", __func__);
		CDBG("%s:slave_addr=0x%x reg_addr=0x%x, data_type=%d\n",
			__func__, read_config.slave_addr,
			read_config.reg_addr, read_config.data_type);
		if (s_ctrl->sensor_i2c_client->cci_client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->cci_client->sid;
			s_ctrl->sensor_i2c_client->cci_client->sid =
				read_slave_addr >> 1;
		} else if (s_ctrl->sensor_i2c_client->client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->client->addr;
			s_ctrl->sensor_i2c_client->client->addr =
				read_slave_addr >> 1;
		} else {
			pr_err("%s: error: no i2c/cci client found.", __func__);
			rc = -EFAULT;
			break;
		}
		CDBG("%s:orig_slave_addr=0x%x, new_slave_addr=0x%x",
				__func__, orig_slave_addr,
				read_slave_addr >> 1);
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_read(
				s_ctrl->sensor_i2c_client,
				read_config.reg_addr,
				&local_data, read_config.data_type);
		if (rc < 0) {
			pr_err("%s:%d: i2c_read failed\n", __func__, __LINE__);
			break;
		}
		if (copy_to_user(&read_config.data,
			(void *)&local_data, sizeof(uint16_t))) {
			pr_err("%s:%d copy failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		break;
	}
	case CFG_WRITE_I2C_SEQ_ARRAY: {
		struct msm_camera_i2c_seq_reg_setting32 conf_array32;
		struct msm_camera_i2c_seq_reg_setting conf_array;
		struct msm_camera_i2c_seq_reg_array *reg_setting = NULL;

		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&conf_array32,
			(void *)compat_ptr(cdata->cfg.setting),
			sizeof(struct msm_camera_i2c_seq_reg_setting32))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		conf_array.addr_type = conf_array32.addr_type;
		conf_array.delay = conf_array32.delay;
		conf_array.size = conf_array32.size;
		conf_array.reg_setting = compat_ptr(conf_array32.reg_setting);

		if (!conf_array.size) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		reg_setting = kzalloc(conf_array.size *
			(sizeof(struct msm_camera_i2c_seq_reg_array)),
			GFP_KERNEL);
		if (!reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(reg_setting, (void *)conf_array.reg_setting,
			conf_array.size *
			sizeof(struct msm_camera_i2c_seq_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}

		conf_array.reg_setting = reg_setting;
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->
			i2c_write_seq_table(s_ctrl->sensor_i2c_client,
			&conf_array);
		kfree(reg_setting);
		break;
	}

	case CFG_POWER_UP:
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_DOWN) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		if (s_ctrl->func_tbl->sensor_power_up) {
			if (s_ctrl->sensordata->misc_regulator)
				msm_sensor_misc_regulator(s_ctrl, 1);

			rc = s_ctrl->func_tbl->sensor_power_up(s_ctrl);
			if (rc < 0) {
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
			s_ctrl->sensor_state = MSM_SENSOR_POWER_UP;
			CDBG("%s:%d sensor state %d\n", __func__, __LINE__,
				s_ctrl->sensor_state);
		} else {
			rc = -EFAULT;
		}
		break;
	case CFG_POWER_DOWN:
		kfree(s_ctrl->stop_setting.reg_setting);
		s_ctrl->stop_setting.reg_setting = NULL;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		/* optimize camera print mipi packet and frame count log*/
		if(read_times > 0) {
			pr_info("%s: cancel frm_cnt_work read_times = %d\n",__func__,read_times);
			read_times = 0;
			cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
		}
		if (s_ctrl->func_tbl->sensor_power_down) {
			if (s_ctrl->sensordata->misc_regulator)
				msm_sensor_misc_regulator(s_ctrl, 0);

			rc = s_ctrl->func_tbl->sensor_power_down(s_ctrl);
			if (rc < 0) {
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
			s_ctrl->sensor_state = MSM_SENSOR_POWER_DOWN;
			CDBG("%s:%d sensor state %d\n", __func__, __LINE__,
				s_ctrl->sensor_state);
		} else {
			rc = -EFAULT;
		}
		break;
	/* optimize camera print mipi packet and frame count log*/
   case CFG_START_FRM_CNT:
       {
           read_times = 0;
           cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
       
           read_times = HW_PRINT_PACKET_NUM_TIME;
           schedule_delayed_work(&s_ctrl->frm_cnt_work, msecs_to_jiffies(HW_READ_PACKET_NUM_TIME));
           pr_info("%s:%s start read frame count work \n",__func__,s_ctrl->sensordata->sensor_name);
       }
       break;

    case CFG_STOP_FRM_CNT:
        {
            read_times = 0;
            cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
            pr_info("%s:%s stop read frame count work \n",__func__,s_ctrl->sensordata->sensor_name);
        }
        break;
	case CFG_SET_STOP_STREAM_SETTING: {
		struct msm_camera_i2c_reg_setting32 stop_setting32;
		struct msm_camera_i2c_reg_setting *stop_setting =
			&s_ctrl->stop_setting;
		struct msm_camera_i2c_reg_array *reg_setting = NULL;
		if (copy_from_user(&stop_setting32,
				(void *)compat_ptr((cdata->cfg.setting)),
			sizeof(struct msm_camera_i2c_reg_setting32))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		stop_setting->addr_type = stop_setting32.addr_type;
		stop_setting->data_type = stop_setting32.data_type;
		stop_setting->delay = stop_setting32.delay;
		stop_setting->size = stop_setting32.size;

		reg_setting = compat_ptr(stop_setting32.reg_setting);

		if (!stop_setting->size) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		stop_setting->reg_setting = kzalloc(stop_setting->size *
			(sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
		if (!stop_setting->reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(stop_setting->reg_setting,
			(void *)reg_setting,
			stop_setting->size *
			sizeof(struct msm_camera_i2c_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(stop_setting->reg_setting);
			stop_setting->reg_setting = NULL;
			stop_setting->size = 0;
			rc = -EFAULT;
			break;
		}
		break;
	}

	case CFG_SET_OTP_INFO:

		CDBG("%s,%d: CFG_SET_OTP_INFO\n", __func__, __LINE__);
		//if power up
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP)
		{
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		//set otp info
		if ( is_exist_otp_function(s_ctrl, &index) )
		{
			rc = otp_function_lists[index].sensor_otp_function(s_ctrl, index);
			if (rc < 0)
			{
				camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_OTP_ERR, rc, NULL);
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
		}
		else
		{
			pr_err("%s, %d: %s unsupport otp operation\n", __func__,
					__LINE__, s_ctrl->sensordata->sensor_name);
		}
		break;

	default:
		rc = -EFAULT;
		break;
	}

	mutex_unlock(s_ctrl->msm_sensor_mutex);

	return rc;
}
#endif

int msm_sensor_config(struct msm_sensor_ctrl_t *s_ctrl, void __user *argp)
{
	struct sensorb_cfg_data *cdata = (struct sensorb_cfg_data *)argp;
	int32_t rc = 0;
	int32_t i = 0;

	int32_t index = -1;
	//delete

	mutex_lock(s_ctrl->msm_sensor_mutex);
	CDBG("%s:%d %s cfgtype = %d\n", __func__, __LINE__,
		s_ctrl->sensordata->sensor_name, cdata->cfgtype);
	switch (cdata->cfgtype) {
	case CFG_GET_SENSOR_INFO:
		memcpy(cdata->cfg.sensor_info.sensor_name,
			s_ctrl->sensordata->sensor_name,
			sizeof(cdata->cfg.sensor_info.sensor_name));
		cdata->cfg.sensor_info.session_id =
			s_ctrl->sensordata->sensor_info->session_id;
		for (i = 0; i < SUB_MODULE_MAX; i++) {
			cdata->cfg.sensor_info.subdev_id[i] =
				s_ctrl->sensordata->sensor_info->subdev_id[i];
			cdata->cfg.sensor_info.subdev_intf[i] =
				s_ctrl->sensordata->sensor_info->subdev_intf[i];
		}
		cdata->cfg.sensor_info.is_mount_angle_valid =
			s_ctrl->sensordata->sensor_info->is_mount_angle_valid;
		cdata->cfg.sensor_info.sensor_mount_angle =
			s_ctrl->sensordata->sensor_info->sensor_mount_angle;
		cdata->cfg.sensor_info.position =
			s_ctrl->sensordata->sensor_info->position;
		cdata->cfg.sensor_info.modes_supported =
			s_ctrl->sensordata->sensor_info->modes_supported;
		CDBG("%s:%d sensor name %s\n", __func__, __LINE__,
			cdata->cfg.sensor_info.sensor_name);
		CDBG("%s:%d session id %d\n", __func__, __LINE__,
			cdata->cfg.sensor_info.session_id);
		for (i = 0; i < SUB_MODULE_MAX; i++) {
			CDBG("%s:%d subdev_id[%d] %d\n", __func__, __LINE__, i,
				cdata->cfg.sensor_info.subdev_id[i]);
			CDBG("%s:%d subdev_intf[%d] %d\n", __func__, __LINE__,
				i, cdata->cfg.sensor_info.subdev_intf[i]);
		}
		CDBG("%s:%d mount angle valid %d value %d\n", __func__,
			__LINE__, cdata->cfg.sensor_info.is_mount_angle_valid,
			cdata->cfg.sensor_info.sensor_mount_angle);

		break;
	case CFG_GET_SENSOR_INIT_PARAMS:
		cdata->cfg.sensor_init_params.modes_supported =
			s_ctrl->sensordata->sensor_info->modes_supported;
		cdata->cfg.sensor_init_params.position =
			s_ctrl->sensordata->sensor_info->position;
		cdata->cfg.sensor_init_params.sensor_mount_angle =
			s_ctrl->sensordata->sensor_info->sensor_mount_angle;
		CDBG("%s:%d init params mode %d pos %d mount %d\n", __func__,
			__LINE__,
			cdata->cfg.sensor_init_params.modes_supported,
			cdata->cfg.sensor_init_params.position,
			cdata->cfg.sensor_init_params.sensor_mount_angle);
		break;

	case CFG_GET_OTP: {
		int8_t otp_index;
		otp_index = get_otp_index(s_ctrl);
		if(otp_index >= 0)
		{
			cdata->cfg.otp_info.otp_vaild = 
				otp_data_lists[otp_index].otp_info->otp_vaild;
			cdata->cfg.otp_info.af_otp.start_code = 
				otp_data_lists[otp_index].otp_info->af_otp.start_code;
			cdata->cfg.otp_info.af_otp.max_code = 
				otp_data_lists[otp_index].otp_info->af_otp.max_code;
			cdata->cfg.otp_info.af_otp.start_dist = 
				otp_data_lists[otp_index].otp_info->af_otp.start_dist;
			cdata->cfg.otp_info.af_otp.end_dist = 
				otp_data_lists[otp_index].otp_info->af_otp.end_dist;
			cdata->cfg.otp_info.af_otp.axis_angle= 
				otp_data_lists[otp_index].otp_info->af_otp.axis_angle;
			cdata->cfg.otp_info.af_otp.pose_offset= 
				otp_data_lists[otp_index].otp_info->af_otp.pose_offset;

			CDBG("%s, %d, otp vaild=%d, start code=%d, max code=%d start dist=%d end dist=%d axis angle=%d pose offset=%d\n",
				__func__, __LINE__,
				otp_data_lists[otp_index].otp_info->otp_vaild,
				otp_data_lists[otp_index].otp_info->af_otp.start_code,
				otp_data_lists[otp_index].otp_info->af_otp.max_code,
				otp_data_lists[otp_index].otp_info->af_otp.start_dist,
				otp_data_lists[otp_index].otp_info->af_otp.end_dist,
				otp_data_lists[otp_index].otp_info->af_otp.axis_angle,
				otp_data_lists[otp_index].otp_info->af_otp.pose_offset);
		}else
			rc = -EFAULT;

		break;
	}

	//delete LOAD_FIRMWARE MISP_STREAMON/MISP_STREAMOFF
	case CFG_WRITE_SPI_ARRAY: {
		struct msm_camera_spi_reg_setting spi_reg;
		uint8_t* param;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&spi_reg,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_spi_reg_setting))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		if (!spi_reg.size) {
			pr_err("%s:%d spi_reg.size = 0\n", __func__, __LINE__);
			rc = -EFAULT;
		}else {
		param = kzalloc(spi_reg.size *sizeof(uint8_t), GFP_KERNEL);
			if (!param) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -ENOMEM;
				break;
			}
			if (copy_from_user(param, (void *)spi_reg.param,
				spi_reg.size *
				sizeof(uint8_t))) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				kfree(param);
				rc = -EFAULT;
				break;
			}
                }

		spi_reg.param = param;
		// send by spi
		misp_write_cmd(spi_reg.opcode,spi_reg.param,spi_reg.size);
		if (spi_reg.size){
			kfree(param);
		}
		break;
	}
	case CFG_SPI_REG_SETTINGS: {
		int j = 0;
		struct msm_camera_spi_reg_settings spi_regs;
		struct msm_camera_spi_array* spi_array;
		uint8_t* param;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		if (copy_from_user(&spi_regs,
			(void*)cdata->cfg.setting,
			sizeof(struct msm_camera_spi_reg_settings))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		if (spi_regs.size <= 0) {
			pr_err("%s:%d spi_regs.size = 0 size can not be 0\n", __func__, __LINE__);
			break;//size can not be 0
		}else {
			spi_array = kzalloc(spi_regs.size *sizeof(struct msm_camera_spi_array), GFP_KERNEL);
			if (!spi_array) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -ENOMEM;
				break;
			}
		}
		for (j = 0;j < spi_regs.size;j++){
			if (copy_from_user(&spi_array[j],
				(void*)(spi_regs.reg_settings+j),
				sizeof(struct msm_camera_spi_array))) {
				pr_err("%s:%d failed\n", __func__, __LINE__);
				rc = -EFAULT;
				goto PARAM_ERROR;
			}
	                pr_err(".size:%d .opcode:%d .is_block_data:%d .is_recv:%d .is_wait_state:%d .wait_state:%d\n",
			spi_array[j].size,
			spi_array[j].opcode,
			spi_array[j].is_block_data,
			spi_array[j].is_recv,
			spi_array[j].is_wait_state,
			spi_array[j].wait_state);
			// get param of spi_array
			if (!spi_array[j].size) {
				pr_err("%s:%d spi_reg.size = 0\n", __func__, __LINE__);
				spi_array[j].param = NULL;
			} else {
				param = kzalloc(spi_array[j].size *sizeof(uint8_t), GFP_KERNEL);
				if (!param) {
					pr_err("%s:%d failed\n", __func__, __LINE__);
					rc = -ENOMEM; //need to release [0-i]'s param
					goto PARAM_ERROR;
				}
				if (copy_from_user(param, (void *)(spi_array[j].param),
					spi_array[j].size *sizeof(uint8_t))) {
					pr_err("%s:%d failed\n", __func__, __LINE__);
					kfree(param);
					rc = -EFAULT;
					goto PARAM_ERROR;
				}
				spi_array[j].param = param;
			}
		}
		// send by spi
		spi_regs.reg_settings = spi_array;
		rc = misp_execmd_array(spi_regs);
PARAM_ERROR:
		j--;
		for (;j >= 0 ;j--){
			if (spi_array[j].size){
				kfree(spi_array[j].param);
			}
		}
		kfree(spi_array);
		break;
		}
	case CFG_WRITE_I2C_ARRAY: {
		struct msm_camera_i2c_reg_setting conf_array;
		struct msm_camera_i2c_reg_array *reg_setting = NULL;

		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&conf_array,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_i2c_reg_setting))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		if (!conf_array.size) {
			pr_err("%s:%d conf_array.size = 0\n", __func__, __LINE__);
			//rc = -EFAULT;
			break;
		}

		reg_setting = kzalloc(conf_array.size *
			(sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
		if (!reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(reg_setting, (void *)conf_array.reg_setting,
			conf_array.size *
			sizeof(struct msm_camera_i2c_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}

		conf_array.reg_setting = reg_setting;
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write_table(
			s_ctrl->sensor_i2c_client, &conf_array);
		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_WRITE_I2C_ARRAY" );
		}
		kfree(reg_setting);
		break;
	}

        case CFG_WRITE_EXPOSURE_DATA: {
            struct msm_camera_i2c_reg_setting conf_array;
            struct msm_camera_i2c_reg_array *reg_setting = NULL;

            if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
                 pr_err("%s:%d failed: invalid state %d\n", __func__,
                     __LINE__, s_ctrl->sensor_state);
                 rc = -EFAULT;
                 break;
            }

            if (copy_from_user(&conf_array,
                 (void *)cdata->cfg.setting,
                 sizeof(struct msm_camera_i2c_reg_setting))) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 rc = -EFAULT;
                 break;
            }

            if (!conf_array.size) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 break;
            }

            reg_setting = kzalloc(conf_array.size *
                 (sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
            if (!reg_setting) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 rc = -ENOMEM;
                 break;
            }
            if (copy_from_user(reg_setting, (void *)conf_array.reg_setting,
                 conf_array.size *
                 sizeof(struct msm_camera_i2c_reg_array))) {
                 pr_err("%s:%d failed\n", __func__, __LINE__);
                 kfree(reg_setting);
                 rc = -EFAULT;
                 break;
            }

            conf_array.reg_setting = reg_setting;
            for (i = 0; i < conf_array.size; i++) {
                if(conf_array.reg_setting[i].data_type){
                   rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write(
                       s_ctrl->sensor_i2c_client, conf_array.reg_setting[i].reg_addr, conf_array.reg_setting[i].reg_data,
                       conf_array.reg_setting[i].data_type);
                }
                else{
                     rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write(
                         s_ctrl->sensor_i2c_client, conf_array.reg_setting[i].reg_addr, conf_array.reg_setting[i].reg_data,
                         conf_array.data_type);
                }
            }
		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_WRITE_EXPOSURE_DATA");
		}
            if (conf_array.delay > 20)
                msleep(conf_array.delay);
            else if (conf_array.delay)
                usleep_range(conf_array.delay * 1000, (conf_array.delay
                  * 1000) + 1000);

            kfree(reg_setting);
            break;
        }

	case CFG_SLAVE_READ_I2C: {
		struct msm_camera_i2c_read_config read_config;
		uint16_t local_data = 0;
		uint16_t orig_slave_addr = 0, read_slave_addr = 0;
		if (copy_from_user(&read_config,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_i2c_read_config))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		read_slave_addr = read_config.slave_addr;
		CDBG("%s:CFG_SLAVE_READ_I2C:", __func__);
		CDBG("%s:slave_addr=0x%x reg_addr=0x%x, data_type=%d\n",
			__func__, read_config.slave_addr,
			read_config.reg_addr, read_config.data_type);
		if (s_ctrl->sensor_i2c_client->cci_client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->cci_client->sid;
			s_ctrl->sensor_i2c_client->cci_client->sid =
				read_slave_addr >> 1;
		} else if (s_ctrl->sensor_i2c_client->client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->client->addr;
			s_ctrl->sensor_i2c_client->client->addr =
				read_slave_addr >> 1;
		} else {
			pr_err("%s: error: no i2c/cci client found.", __func__);
			rc = -EFAULT;
			break;
		}
		CDBG("%s:orig_slave_addr=0x%x, new_slave_addr=0x%x",
				__func__, orig_slave_addr,
				read_slave_addr >> 1);
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_read(
				s_ctrl->sensor_i2c_client,
				read_config.reg_addr,
				&local_data, read_config.data_type);
		if (rc < 0) {
			pr_err("%s:%d: i2c_read failed\n", __func__, __LINE__);
			//read err
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_SLAVE_READ_I2C");
			break;
		}
		if (copy_to_user(&read_config.data,
			(void *)&local_data, sizeof(uint16_t))) {
			pr_err("%s:%d copy failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		break;
	}
	case CFG_SLAVE_WRITE_I2C_ARRAY: {
		struct msm_camera_i2c_array_write_config write_config;
		struct msm_camera_i2c_reg_array *reg_setting = NULL;
		uint16_t write_slave_addr = 0;
		uint16_t orig_slave_addr = 0;

		if (copy_from_user(&write_config,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_i2c_array_write_config))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		CDBG("%s:CFG_SLAVE_WRITE_I2C_ARRAY:", __func__);
		CDBG("%s:slave_addr=0x%x, array_size=%d\n", __func__,
			write_config.slave_addr,
			write_config.conf_array.size);

		if (!write_config.conf_array.size) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		reg_setting = kzalloc(write_config.conf_array.size *
			(sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
		if (!reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(reg_setting,
				(void *)(write_config.conf_array.reg_setting),
				write_config.conf_array.size *
				sizeof(struct msm_camera_i2c_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}
		write_config.conf_array.reg_setting = reg_setting;
		write_slave_addr = write_config.slave_addr;
		if (s_ctrl->sensor_i2c_client->cci_client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->cci_client->sid;
			s_ctrl->sensor_i2c_client->cci_client->sid =
				write_slave_addr >> 1;
		} else if (s_ctrl->sensor_i2c_client->client) {
			orig_slave_addr =
				s_ctrl->sensor_i2c_client->client->addr;
			s_ctrl->sensor_i2c_client->client->addr =
				write_slave_addr >> 1;
		} else {
			pr_err("%s: error: no i2c/cci client found.", __func__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}
		CDBG("%s:orig_slave_addr=0x%x, new_slave_addr=0x%x",
				__func__, orig_slave_addr,
				write_slave_addr >> 1);
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->i2c_write_table(
			s_ctrl->sensor_i2c_client, &(write_config.conf_array));
		if (s_ctrl->sensor_i2c_client->cci_client) {
			s_ctrl->sensor_i2c_client->cci_client->sid =
				orig_slave_addr;
		} else if (s_ctrl->sensor_i2c_client->client) {
			s_ctrl->sensor_i2c_client->client->addr =
				orig_slave_addr;
		} else {
			pr_err("%s: error: no i2c/cci client found.", __func__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}

		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_SLAVE_WRITE_I2C_ARRAY");
		}
		
		kfree(reg_setting);
		break;
	}
	case CFG_WRITE_I2C_SEQ_ARRAY: {
		struct msm_camera_i2c_seq_reg_setting conf_array;
		struct msm_camera_i2c_seq_reg_array *reg_setting = NULL;

		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}

		if (copy_from_user(&conf_array,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_i2c_seq_reg_setting))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		if (!conf_array.size) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		reg_setting = kzalloc(conf_array.size *
			(sizeof(struct msm_camera_i2c_seq_reg_array)),
			GFP_KERNEL);
		if (!reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(reg_setting, (void *)conf_array.reg_setting,
			conf_array.size *
			sizeof(struct msm_camera_i2c_seq_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(reg_setting);
			rc = -EFAULT;
			break;
		}

		conf_array.reg_setting = reg_setting;
		rc = s_ctrl->sensor_i2c_client->i2c_func_tbl->
			i2c_write_seq_table(s_ctrl->sensor_i2c_client,
			&conf_array);

		//i2c write err
		if ( rc < 0 )
		{
			camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_I2C_ERR, rc, "CFG_WRITE_I2C_SEQ_ARRAY");
		}
		
		kfree(reg_setting);
		break;
	}

	case CFG_POWER_UP:
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_DOWN) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		if (s_ctrl->func_tbl->sensor_power_up) {
			if (s_ctrl->sensordata->misc_regulator)
				msm_sensor_misc_regulator(s_ctrl, 1);

			rc = s_ctrl->func_tbl->sensor_power_up(s_ctrl);
			if (rc < 0) {
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
			s_ctrl->sensor_state = MSM_SENSOR_POWER_UP;
			pr_err("%s:%d sensor state %d\n", __func__, __LINE__,
				s_ctrl->sensor_state);
		} else {
			rc = -EFAULT;
		}
		break;

	case CFG_POWER_DOWN:
		kfree(s_ctrl->stop_setting.reg_setting);
		s_ctrl->stop_setting.reg_setting = NULL;
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP) {
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		/* optimize camera print mipi packet and frame count log*/
		if(read_times > 0) {
			pr_info("%s: cancel frm_cnt_work read_times = %d\n",__func__,read_times);
			read_times = 0;
			cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
		}
		if (s_ctrl->func_tbl->sensor_power_down) {
			if (s_ctrl->sensordata->misc_regulator)
				msm_sensor_misc_regulator(s_ctrl, 0);

			rc = s_ctrl->func_tbl->sensor_power_down(s_ctrl);
			if (rc < 0) {
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
			s_ctrl->sensor_state = MSM_SENSOR_POWER_DOWN;
			pr_err("%s:%d sensor state %d\n", __func__, __LINE__,
				s_ctrl->sensor_state);
		} else {
			rc = -EFAULT;
		}
		break;
	/* optimize camera print mipi packet and frame count log*/
   case CFG_START_FRM_CNT:
       {
           read_times = 0;
           cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
       
           read_times = HW_PRINT_PACKET_NUM_TIME;
           schedule_delayed_work(&s_ctrl->frm_cnt_work, msecs_to_jiffies(HW_READ_PACKET_NUM_TIME));
           pr_info("%s:%s start read frame count work \n",__func__,s_ctrl->sensordata->sensor_name);
       }
       break;

    case CFG_STOP_FRM_CNT:
        {
            read_times = 0;
            cancel_delayed_work_sync(&s_ctrl->frm_cnt_work);
            pr_info("%s:%s stop read frame count work \n",__func__,s_ctrl->sensordata->sensor_name);
        }
        break;
	case CFG_SET_STOP_STREAM_SETTING: {
		struct msm_camera_i2c_reg_setting *stop_setting =
			&s_ctrl->stop_setting;
		struct msm_camera_i2c_reg_array *reg_setting = NULL;
		if (copy_from_user(stop_setting,
			(void *)cdata->cfg.setting,
			sizeof(struct msm_camera_i2c_reg_setting))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}

		reg_setting = stop_setting->reg_setting;

		if (!stop_setting->size) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EFAULT;
			break;
		}
		stop_setting->reg_setting = kzalloc(stop_setting->size *
			(sizeof(struct msm_camera_i2c_reg_array)), GFP_KERNEL);
		if (!stop_setting->reg_setting) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -ENOMEM;
			break;
		}
		if (copy_from_user(stop_setting->reg_setting,
			(void *)reg_setting,
			stop_setting->size *
			sizeof(struct msm_camera_i2c_reg_array))) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			kfree(stop_setting->reg_setting);
			stop_setting->reg_setting = NULL;
			stop_setting->size = 0;
			rc = -EFAULT;
			break;
		}
		break;
	}

	case CFG_SET_OTP_INFO:

		CDBG("%s,%d: CFG_SET_OTP_INFO\n", __func__, __LINE__);
		//if power up
		if (s_ctrl->sensor_state != MSM_SENSOR_POWER_UP)
		{
			pr_err("%s:%d failed: invalid state %d\n", __func__,
				__LINE__, s_ctrl->sensor_state);
			rc = -EFAULT;
			break;
		}
		//set otp info
		if ( is_exist_otp_function(s_ctrl, &index) )
		{
			rc = otp_function_lists[index].sensor_otp_function(s_ctrl, index);
			if (rc < 0)
			{
				camera_report_dsm_err_msm_sensor(s_ctrl, DSM_CAMERA_OTP_ERR, rc, NULL);
				pr_err("%s:%d failed rc %d\n", __func__,
					__LINE__, rc);
				break;
			}
		}
		else
		{
			pr_err("%s, %d: %s unsupport otp operation\n", __func__,
					__LINE__, s_ctrl->sensordata->sensor_name);
		}
		break;

	default:
		rc = -EFAULT;
		break;
	}

	mutex_unlock(s_ctrl->msm_sensor_mutex);

	return rc;
}

int msm_sensor_check_id(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc;

	if (s_ctrl->func_tbl->sensor_match_id)
		rc = s_ctrl->func_tbl->sensor_match_id(s_ctrl);
	else
		rc = msm_sensor_match_id_misp(s_ctrl);
	if (rc < 0)
		pr_err("%s:%d match id failed rc %d\n", __func__, __LINE__, rc);
	return rc;
}

static int msm_sensor_power(struct v4l2_subdev *sd, int on)
{
	int rc = 0;
	struct msm_sensor_ctrl_t *s_ctrl = get_sctrl(sd);
	mutex_lock(s_ctrl->msm_sensor_mutex);
	if (!on && s_ctrl->sensor_state == MSM_SENSOR_POWER_UP) {
		s_ctrl->func_tbl->sensor_power_down(s_ctrl);
		s_ctrl->sensor_state = MSM_SENSOR_POWER_DOWN;
	}
	mutex_unlock(s_ctrl->msm_sensor_mutex);
	return rc;
}

static int msm_sensor_v4l2_enum_fmt(struct v4l2_subdev *sd,
	unsigned int index, enum v4l2_mbus_pixelcode *code)
{
	struct msm_sensor_ctrl_t *s_ctrl = get_sctrl(sd);

	if ((unsigned int)index >= s_ctrl->sensor_v4l2_subdev_info_size)
		return -EINVAL;

	*code = s_ctrl->sensor_v4l2_subdev_info[index].code;
	return 0;
}

void camera_report_dsm_err_msm_sensor(struct msm_sensor_ctrl_t *s_ctrl, int type, int err_num , const char* str)
{
#ifdef CONFIG_HUAWEI_DSM
	ssize_t len = 0;

	memset(camera_dsm_log_buff, 0, MSM_SENSOR_BUFFER_SIZE);

	if ( (NULL != s_ctrl) && (NULL != s_ctrl->sensordata) )
	{
		//get module info
		len = snprintf(camera_dsm_log_buff,
				MSM_SENSOR_BUFFER_SIZE, 
				"Sensor name:%s, eeprom name:%s ",
				s_ctrl->sensordata->sensor_name,
				s_ctrl->sensordata->eeprom_name);
	}

	if ( len >= MSM_SENSOR_BUFFER_SIZE -1 )
	{
		CDBG("write camera_dsm_log_buff overflow.\n");
		return;
	}
	
	/* camera record error info according to err type */
	switch(type)
	{
		case DSM_CAMERA_I2C_ERR:
			/* report i2c infomation */
			len += snprintf(camera_dsm_log_buff+len, MSM_SENSOR_BUFFER_SIZE-len, "[msm_sensor]I2C Error : %s\n.", str);
			break;

		case DSM_CAMERA_OTP_ERR:
			/* report otp infomation */
			len += snprintf(camera_dsm_log_buff+len, MSM_SENSOR_BUFFER_SIZE-len, "[msm_sensor]OTP error.No effective OTP info.\n");
			break;

		case DSM_CAMERA_CHIP_ID_NOT_MATCH:
			/* report otp infomation */
			len += snprintf(camera_dsm_log_buff+len, MSM_SENSOR_BUFFER_SIZE-len, "[msm_sensor]Chip ID DON'T MATCH.\n");
			break;

		default:
			break;
	}
	
	camera_report_dsm_err( type, err_num, camera_dsm_log_buff);
#endif
}

static struct v4l2_subdev_core_ops msm_sensor_subdev_core_ops = {
	.ioctl = msm_sensor_subdev_ioctl,
	.s_power = msm_sensor_power,
};

static struct v4l2_subdev_video_ops msm_sensor_subdev_video_ops = {
	.enum_mbus_fmt = msm_sensor_v4l2_enum_fmt,
};

static struct v4l2_subdev_ops msm_sensor_subdev_ops = {
	.core = &msm_sensor_subdev_core_ops,
	.video  = &msm_sensor_subdev_video_ops,
};

static struct msm_sensor_fn_t msm_sensor_func_tbl = {
	.sensor_config = msm_sensor_config,
#ifdef CONFIG_COMPAT
	.sensor_config32 = msm_sensor_config32,
#endif
	.sensor_power_up = msm_sensor_power_up,
	.sensor_power_down = msm_sensor_power_down,
	.sensor_match_id = msm_sensor_match_id_misp,
	/* optimize camera print mipi packet and frame count log*/
	.sensor_read_framecount = hw_sensor_read_framecount,
};

static struct msm_camera_i2c_fn_t msm_sensor_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_write_conf_tbl = msm_camera_cci_i2c_write_conf_tbl,
};

static struct msm_camera_i2c_fn_t msm_sensor_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_qup_i2c_write_table_w_microdelay,
	.i2c_write_conf_tbl = msm_camera_qup_i2c_write_conf_tbl,
};

int32_t msm_sensor_platform_probe(struct platform_device *pdev,
				  const void *data)
{
	int rc = 0;
	struct msm_sensor_ctrl_t *s_ctrl =
		(struct msm_sensor_ctrl_t *)data;
	struct msm_camera_cci_client *cci_client = NULL;
	uint32_t session_id;
	unsigned long mount_pos = 0;
	s_ctrl->pdev = pdev;
	CDBG("%s called data %p\n", __func__, data);
	CDBG("%s pdev name %s\n", __func__, pdev->id_entry->name);
	if (pdev->dev.of_node) {
		rc = msm_sensor_get_dt_data(pdev->dev.of_node, s_ctrl);
		if (rc < 0) {
			pr_err("%s failed line %d\n", __func__, __LINE__);
			return rc;
		}
	}
	s_ctrl->sensordata->power_info.dev = &pdev->dev;
	s_ctrl->sensor_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	s_ctrl->sensor_i2c_client->cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!s_ctrl->sensor_i2c_client->cci_client) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		return rc;
	}
	/* TODO: get CCI subdev */
	cci_client = s_ctrl->sensor_i2c_client->cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = s_ctrl->cci_i2c_master;
	cci_client->sid =
		s_ctrl->sensordata->slave_info->sensor_slave_addr >> 1;
	cci_client->retries = 3;
	cci_client->id_map = 0;
	if (!s_ctrl->func_tbl)
		s_ctrl->func_tbl = &msm_sensor_func_tbl;
	if (!s_ctrl->sensor_i2c_client->i2c_func_tbl)
		s_ctrl->sensor_i2c_client->i2c_func_tbl =
			&msm_sensor_cci_func_tbl;
	if (!s_ctrl->sensor_v4l2_subdev_ops)
		s_ctrl->sensor_v4l2_subdev_ops = &msm_sensor_subdev_ops;
	s_ctrl->sensordata->power_info.clk_info =
		kzalloc(sizeof(cam_8974_clk_info), GFP_KERNEL);
	if (!s_ctrl->sensordata->power_info.clk_info) {
		pr_err("%s:%d failed nomem\n", __func__, __LINE__);
		kfree(cci_client);
		return -ENOMEM;
	}
	memcpy(s_ctrl->sensordata->power_info.clk_info, cam_8974_clk_info,
		sizeof(cam_8974_clk_info));
	s_ctrl->sensordata->power_info.clk_info_size =
		ARRAY_SIZE(cam_8974_clk_info);
	rc = s_ctrl->func_tbl->sensor_power_up(s_ctrl);
	if (rc < 0) {
		pr_err("%s %s power up failed\n", __func__,
			s_ctrl->sensordata->sensor_name);
		kfree(s_ctrl->sensordata->power_info.clk_info);
		kfree(cci_client);
		return rc;
	}

	pr_info("%s %s probe succeeded\n", __func__,
		s_ctrl->sensordata->sensor_name);
	v4l2_subdev_init(&s_ctrl->msm_sd.sd,
		s_ctrl->sensor_v4l2_subdev_ops);
	snprintf(s_ctrl->msm_sd.sd.name,
		sizeof(s_ctrl->msm_sd.sd.name), "%s",
		s_ctrl->sensordata->sensor_name);
	v4l2_set_subdevdata(&s_ctrl->msm_sd.sd, pdev);
	s_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&s_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	s_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	s_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_SENSOR;
	s_ctrl->msm_sd.sd.entity.name =
		s_ctrl->msm_sd.sd.name;

	mount_pos = s_ctrl->sensordata->sensor_info->position << 16;
	mount_pos = mount_pos | ((s_ctrl->sensordata->sensor_info->
					sensor_mount_angle / 90) << 8);
	s_ctrl->msm_sd.sd.entity.flags = mount_pos | MEDIA_ENT_FL_DEFAULT;

	rc = camera_init_v4l2(&s_ctrl->pdev->dev, &session_id);
	CDBG("%s rc %d session_id %d\n", __func__, rc, session_id);
	s_ctrl->sensordata->sensor_info->session_id = session_id;
	s_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x3;
	msm_sd_register(&s_ctrl->msm_sd);
	msm_sensor_v4l2_subdev_fops = v4l2_subdev_fops;
#ifdef CONFIG_COMPAT
	msm_sensor_v4l2_subdev_fops.compat_ioctl32 =
		msm_sensor_subdev_fops_ioctl;
#endif
	s_ctrl->msm_sd.sd.devnode->fops =
		&msm_sensor_v4l2_subdev_fops;

	CDBG("%s:%d\n", __func__, __LINE__);

	s_ctrl->func_tbl->sensor_power_down(s_ctrl);
	CDBG("%s:%d\n", __func__, __LINE__);
	return rc;
}

int msm_sensor_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id, struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	uint32_t session_id;
	unsigned long mount_pos = 0;
	CDBG("%s %s_i2c_probe called\n", __func__, client->name);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s %s i2c_check_functionality failed\n",
			__func__, client->name);
		rc = -EFAULT;
		return rc;
	}

	if (!client->dev.of_node) {
		CDBG("msm_sensor_i2c_probe: of_node is NULL");
		s_ctrl = (struct msm_sensor_ctrl_t *)(id->driver_data);
		if (!s_ctrl) {
			pr_err("%s:%d sensor ctrl structure NULL\n", __func__,
				__LINE__);
			return -EINVAL;
		}
		s_ctrl->sensordata = client->dev.platform_data;
	} else {
		CDBG("msm_sensor_i2c_probe: of_node exisists");
		rc = msm_sensor_get_dt_data(client->dev.of_node, s_ctrl);
		if (rc < 0) {
			pr_err("%s failed line %d\n", __func__, __LINE__);
			return rc;
		}
	}

	s_ctrl->sensor_device_type = MSM_CAMERA_I2C_DEVICE;
	if (s_ctrl->sensordata == NULL) {
		pr_err("%s %s NULL sensor data\n", __func__, client->name);
		return -EFAULT;
	}

	if (s_ctrl->sensor_i2c_client != NULL) {
		s_ctrl->sensor_i2c_client->client = client;
		s_ctrl->sensordata->power_info.dev = &client->dev;
		if (s_ctrl->sensordata->slave_info->sensor_slave_addr)
			s_ctrl->sensor_i2c_client->client->addr =
				s_ctrl->sensordata->slave_info->
				sensor_slave_addr;
	} else {
		pr_err("%s %s sensor_i2c_client NULL\n",
			__func__, client->name);
		rc = -EFAULT;
		return rc;
	}

	if (!s_ctrl->func_tbl)
		s_ctrl->func_tbl = &msm_sensor_func_tbl;
	if (!s_ctrl->sensor_i2c_client->i2c_func_tbl)
		s_ctrl->sensor_i2c_client->i2c_func_tbl =
			&msm_sensor_qup_func_tbl;
	if (!s_ctrl->sensor_v4l2_subdev_ops)
		s_ctrl->sensor_v4l2_subdev_ops = &msm_sensor_subdev_ops;

	if (!client->dev.of_node) {
		s_ctrl->sensordata->power_info.clk_info =
			kzalloc(sizeof(cam_8960_clk_info), GFP_KERNEL);
		if (!s_ctrl->sensordata->power_info.clk_info) {
			pr_err("%s:%d failed nomem\n", __func__, __LINE__);
			return -ENOMEM;
		}
		memcpy(s_ctrl->sensordata->power_info.clk_info,
			cam_8960_clk_info, sizeof(cam_8960_clk_info));
		s_ctrl->sensordata->power_info.clk_info_size =
			ARRAY_SIZE(cam_8960_clk_info);
	} else {
		s_ctrl->sensordata->power_info.clk_info =
			kzalloc(sizeof(cam_8610_clk_info), GFP_KERNEL);
		if (!s_ctrl->sensordata->power_info.clk_info) {
			pr_err("%s:%d failed nomem\n", __func__, __LINE__);
			return -ENOMEM;
		}
		memcpy(s_ctrl->sensordata->power_info.clk_info,
			cam_8610_clk_info, sizeof(cam_8610_clk_info));
		s_ctrl->sensordata->power_info.clk_info_size =
			ARRAY_SIZE(cam_8610_clk_info);
	}

	rc = s_ctrl->func_tbl->sensor_power_up(s_ctrl);
	if (rc < 0) {
		pr_err("%s %s power up failed\n", __func__, client->name);
		kfree(s_ctrl->sensordata->power_info.clk_info);
		return rc;
	}

	CDBG("%s %s probe succeeded\n", __func__, client->name);
	snprintf(s_ctrl->msm_sd.sd.name,
		sizeof(s_ctrl->msm_sd.sd.name), "%s", id->name);
	v4l2_i2c_subdev_init(&s_ctrl->msm_sd.sd, client,
		s_ctrl->sensor_v4l2_subdev_ops);
	v4l2_set_subdevdata(&s_ctrl->msm_sd.sd, client);
	s_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&s_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	s_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	s_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_SENSOR;
	s_ctrl->msm_sd.sd.entity.name =
		s_ctrl->msm_sd.sd.name;
	mount_pos = s_ctrl->sensordata->sensor_info->position << 16;
	mount_pos = mount_pos | ((s_ctrl->sensordata->sensor_info->
					sensor_mount_angle / 90) << 8);
	s_ctrl->msm_sd.sd.entity.flags = mount_pos | MEDIA_ENT_FL_DEFAULT;

	rc = camera_init_v4l2(&s_ctrl->sensor_i2c_client->client->dev,
		&session_id);
	CDBG("%s rc %d session_id %d\n", __func__, rc, session_id);
	s_ctrl->sensordata->sensor_info->session_id = session_id;
	s_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x3;
	msm_sd_register(&s_ctrl->msm_sd);
	CDBG("%s:%d\n", __func__, __LINE__);

	s_ctrl->func_tbl->sensor_power_down(s_ctrl);
	return rc;
}

int32_t msm_sensor_init_default_params(struct msm_sensor_ctrl_t *s_ctrl)
{
	int32_t                       rc = -ENOMEM;
	struct msm_camera_cci_client *cci_client = NULL;
	struct msm_cam_clk_info      *clk_info = NULL;
	unsigned long mount_pos = 0;

	/* Validate input parameters */
	if (!s_ctrl) {
		pr_err("%s:%d failed: invalid params s_ctrl %p\n", __func__,
			__LINE__, s_ctrl);
		return -EINVAL;
	}

	if (!s_ctrl->sensor_i2c_client) {
		pr_err("%s:%d failed: invalid params sensor_i2c_client %p\n",
			__func__, __LINE__, s_ctrl->sensor_i2c_client);
		return -EINVAL;
	}

	/* Initialize cci_client */
	s_ctrl->sensor_i2c_client->cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!s_ctrl->sensor_i2c_client->cci_client) {
		pr_err("%s:%d failed: no memory cci_client %p\n", __func__,
			__LINE__, s_ctrl->sensor_i2c_client->cci_client);
		return -ENOMEM;
	}

	if (s_ctrl->sensor_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		cci_client = s_ctrl->sensor_i2c_client->cci_client;

		/* Get CCI subdev */
		cci_client->cci_subdev = msm_cci_get_subdev();

		/* Update CCI / I2C function table */
		if (!s_ctrl->sensor_i2c_client->i2c_func_tbl)
			s_ctrl->sensor_i2c_client->i2c_func_tbl =
				&msm_sensor_cci_func_tbl;
	} else {
		if (!s_ctrl->sensor_i2c_client->i2c_func_tbl) {
			CDBG("%s:%d\n", __func__, __LINE__);
			s_ctrl->sensor_i2c_client->i2c_func_tbl =
				&msm_sensor_qup_func_tbl;
		}
	}

	/* Update function table driven by ioctl */
	if (!s_ctrl->func_tbl)
		s_ctrl->func_tbl = &msm_sensor_func_tbl;

	/* Update v4l2 subdev ops table */
	if (!s_ctrl->sensor_v4l2_subdev_ops)
		s_ctrl->sensor_v4l2_subdev_ops = &msm_sensor_subdev_ops;

	/* Initialize clock info */
	clk_info = kzalloc(sizeof(cam_8974_clk_info), GFP_KERNEL);
	if (!clk_info) {
		pr_err("%s:%d failed no memory clk_info %p\n", __func__,
			__LINE__, clk_info);
		rc = -ENOMEM;
		goto FREE_CCI_CLIENT;
	}
	memcpy(clk_info, cam_8974_clk_info, sizeof(cam_8974_clk_info));
	s_ctrl->sensordata->power_info.clk_info = clk_info;
	s_ctrl->sensordata->power_info.clk_info_size =
		ARRAY_SIZE(cam_8974_clk_info);

	/* Update sensor mount angle and position in media entity flag */
	mount_pos = s_ctrl->sensordata->sensor_info->position << 16;
	mount_pos = mount_pos | ((s_ctrl->sensordata->sensor_info->
					sensor_mount_angle / 90) << 8);
	s_ctrl->msm_sd.sd.entity.flags = mount_pos | MEDIA_ENT_FL_DEFAULT;

	/* optimize camera print mipi packet and frame count log*/
	INIT_DELAYED_WORK(&s_ctrl->frm_cnt_work, read_framecount_work_handler);

	return 0;

FREE_CCI_CLIENT:
	kfree(cci_client);
	return rc;
}
