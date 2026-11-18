/* SPDX-License-Identifier: GPL-2.0 */
/*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <linux/gpio/consumer.h>
#include <linux/delay.h>

#ifndef _bias_SW_H_
#define _bias_SW_H_

#define BIAS_READ_VENDOR 0x04
#define BIAS_IC_AWINIC   0x01

struct bias_setting_table {
	unsigned char cmd;
	unsigned char data;
};

void bias_on(struct gpio_desc *avdd, struct gpio_desc *avee);
void bias_off(struct gpio_desc *avdd, struct gpio_desc *avee);
#endif
