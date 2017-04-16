/*
 * Keyboard class input driver for the NVIDIA Tegra SoC internal matrix
 * keyboard controller
 *
 * Copyright (c) 2009-2011, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/input/tegra_kbc.h>
#include <linux/clk/tegra.h>
#include <linux/err.h>

#define KBC_MAX_DEBOUNCE_CNT	0x3ffu

/* KBC row scan time and delay for beginning the row scan. */
#define KBC_ROW_SCAN_TIME	16
#define KBC_ROW_SCAN_DLY	5

/* KBC uses a 32KHz clock so a cycle = 1/32Khz */
#define KBC_CYCLE_MS	32

/* KBC Registers */

/* KBC Control Register */
#define KBC_CONTROL_0	0x0
#define KBC_FIFO_TH_CNT_SHIFT(cnt)	(cnt << 14)
#define KBC_DEBOUNCE_CNT_SHIFT(cnt)	(cnt << 4)
#define KBC_CONTROL_FIFO_CNT_INT_EN	(1 << 3)
#define KBC_CONTROL_KEYPRESS_INT_EN	(1 << 1)
#define KBC_CONTROL_KBC_EN		(1 << 0)
#define KBC_CONTROL_KP_INT_EN		(1<<1)

/* KBC Interrupt Register */
#define KBC_INT_0	0x4
#define KBC_INT_FIFO_CNT_INT_STATUS	(1 << 2)
#define KBC_INT_KEYPRESS_INT_STATUS	(1 << 0)

#define KBC_ROW_CFG0_0	0x8
#define KBC_COL_CFG0_0	0x18
#define KBC_TO_CNT_0	0x24
#define KBC_INIT_DLY_0	0x28
#define KBC_RPT_DLY_0	0x2c
#define KBC_KP_ENT0_0	0x30
#define KBC_KP_ENT1_0	0x34
#define KBC_ROW0_MASK_0	0x38

#define KBC_ROW_SHIFT	3
#define DEFAULT_SCAN_COUNT 2
#define DEFAULT_INIT_DLY   5

struct tegra_kbc {
	void __iomem *mmio;
	struct input_dev *idev;
	unsigned int irq;
	unsigned int wake_enable_rows;
	unsigned int wake_enable_cols;
	spinlock_t lock;
	unsigned int repoll_dly;
	unsigned long cp_dly_jiffies;
	unsigned int cp_to_wkup_dly;
	bool use_fn_map;
	bool use_ghost_filter;
	bool keypress_caused_wake;
	const struct tegra_kbc_platform_data *pdata;
	unsigned short keycode[KBC_MAX_KEY * 2];
	unsigned short current_keys[KBC_MAX_KPENT];
	unsigned int num_pressed_keys;
	u32 wakeup_key;
	struct timer_list timer;
	struct clk *clk;
	int is_open;
	unsigned long scan_timeout_count;
	unsigned long one_scan_time;
};

static void tegra_kbc_report_released_keys(struct input_dev *input,
					   unsigned short old_keycodes[],
					   unsigned int old_num_keys,
					   unsigned short new_keycodes[],
					   unsigned int new_num_keys)
{
	unsigned int i, j;

	for (i = 0; i < old_num_keys; i++) {
		for (j = 0; j < new_num_keys; j++)
			if (old_keycodes[i] == new_keycodes[j])
				break;

		if (j == new_num_keys)
			input_report_key(input, old_keycodes[i], 0);
	}
}

static void tegra_kbc_report_pressed_keys(struct input_dev *input,
					  unsigned char scancodes[],
					  unsigned short keycodes[],
					  unsigned int num_pressed_keys)
{
	unsigned int i;

	for (i = 0; i < num_pressed_keys; i++) {
		input_event(input, EV_MSC, MSC_SCAN, scancodes[i]);
		input_report_key(input, keycodes[i], 1);
	}
}

static void tegra_kbc_report_keys(struct tegra_kbc *kbc)
{
	unsigned char scancodes[KBC_MAX_KPENT];
	unsigned short keycodes[KBC_MAX_KPENT];
	u32 val = 0;
	unsigned int i;
	unsigned int num_down = 0;
	bool fn_keypress = false;
	bool key_in_same_row = false;
	bool key_in_same_col = false;

	for (i = 0; i < KBC_MAX_KPENT; i++) {
		if ((i % 4) == 0)
			val = readl(kbc->mmio + KBC_KP_ENT0_0 + i);

		if (val & 0x80) {
			unsigned int col = val & 0x07;
			unsigned int row = (val >> 3) & 0x0f;
			unsigned char scancode =
				MATRIX_SCAN_CODE(row, col, KBC_ROW_SHIFT);

			scancodes[num_down] = scancode;
			keycodes[num_down] = kbc->keycode[scancode];
			/* If driver uses Fn map, do not report the Fn key. */
			if ((keycodes[num_down] == KEY_FN) && kbc->use_fn_map)
				fn_keypress = true;
			else
				num_down++;
		}

		val >>= 8;
	}

	/*
	 * Matrix keyboard designs are prone to keyboard ghosting.
	 * Ghosting occurs if there are 3 keys such that -
	 * any 2 of the 3 keys share a row, and any 2 of them share a column.
	 * If so ignore the key presses for this iteration.
	 */
	if (kbc->use_ghost_filter && num_down >= 3) {
		for (i = 0; i < num_down; i++) {
			unsigned int j;
			u8 curr_col = scancodes[i] & 0x07;
			u8 curr_row = scancodes[i] >> KBC_ROW_SHIFT;

			/*
			 * Find 2 keys such that one key is in the same row
			 * and the other is in the same column as the i-th key.
			 */
			for (j = i + 1; j < num_down; j++) {
				u8 col = scancodes[j] & 0x07;
				u8 row = scancodes[j] >> KBC_ROW_SHIFT;

				if (col == curr_col)
					key_in_same_col = true;
				if (row == curr_row)
					key_in_same_row = true;
			}
		}
	}

	/*
	 * If the platform uses Fn keymaps, translate keys on a Fn keypress.
	 * Function keycodes are KBC_MAX_KEY apart from the plain keycodes.
	 */
	if (fn_keypress) {
		for (i = 0; i < num_down; i++) {
			scancodes[i] += KBC_MAX_KEY;
			keycodes[i] = kbc->keycode[scancodes[i]];
		}
	}

	/* Ignore the key presses for this iteration? */
	if (key_in_same_col && key_in_same_row)
		return;

	tegra_kbc_report_released_keys(kbc->idev,
				       kbc->current_keys, kbc->num_pressed_keys,
				       keycodes, num_down);
	tegra_kbc_report_pressed_keys(kbc->idev, scancodes, keycodes, num_down);
	input_sync(kbc->idev);

	memcpy(kbc->current_keys, keycodes, sizeof(kbc->current_keys));
	kbc->num_pressed_keys = num_down;
}

static void tegra_kbc_set_fifo_interrupt(struct tegra_kbc *kbc, bool enable)
{
	u32 val;

	val = readl(kbc->mmio + KBC_CONTROL_0);
	if (enable)
		val |= KBC_CONTROL_FIFO_CNT_INT_EN;
	else
		val &= ~KBC_CONTROL_FIFO_CNT_INT_EN;
	writel(val, kbc->mmio + KBC_CONTROL_0);
}

static void tegra_kbc_keypress_timer(unsigned long data)
{
	struct tegra_kbc *kbc = (struct tegra_kbc *)data;
	unsigned long flags;
	u32 val;
	unsigned int i;

	spin_lock_irqsave(&kbc->lock, flags);

	val = (readl(kbc->mmio + KBC_INT_0) >> 4) & 0xf;
	if (val) {
		unsigned long dly;

		tegra_kbc_report_keys(kbc);

		/*
		 * If more than one keys are pressed we need not wait
		 * for the repoll delay.
		 */
		dly = (val == 1) ? kbc->repoll_dly : 1;
		mod_timer(&kbc->timer, jiffies + msecs_to_jiffies(dly));
	} else {
		/* Release any pressed keys and exit the polling loop */
		for (i = 0; i < kbc->num_pressed_keys; i++)
			input_report_key(kbc->idev, kbc->current_keys[i], 0);
		input_sync(kbc->idev);

		kbc->num_pressed_keys = 0;

		/* All keys are released so enable the keypress interrupt */
		tegra_kbc_set_fifo_interrupt(kbc, true);
	}

	spin_unlock_irqrestore(&kbc->lock, flags);
}

static irqreturn_t tegra_kbc_isr(int irq, void *args)
{
	struct tegra_kbc *kbc = args;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&kbc->lock, flags);

	/*
	 * Quickly bail out & reenable interrupts if the fifo threshold
	 * count interrupt wasn't the interrupt source
	 */
	val = readl(kbc->mmio + KBC_INT_0);
	writel(val, kbc->mmio + KBC_INT_0);

	if (val & KBC_INT_FIFO_CNT_INT_STATUS) {
		/*
		 * Until all keys are released, defer further processing to
		 * the polling loop in tegra_kbc_keypress_timer.
		 */
		tegra_kbc_set_fifo_interrupt(kbc, false);
		mod_timer(&kbc->timer, jiffies + kbc->cp_dly_jiffies);
	} else if (val & KBC_INT_KEYPRESS_INT_STATUS) {
		/* We can be here only through system resume path */
		kbc->keypress_caused_wake = true;
	}

	spin_unlock_irqrestore(&kbc->lock, flags);

	return IRQ_HANDLED;
}

static void tegra_kbc_setup_wakekeys(struct tegra_kbc *kbc, bool filter)
{
	const struct tegra_kbc_platform_data *pdata = kbc->pdata;
	int i;
	unsigned int rst_val;

	BUG_ON(pdata->wake_cnt > KBC_MAX_KEY);
	rst_val = (filter && pdata->wake_cnt) ? ~0 : 0;

	for (i = 0; i < KBC_MAX_ROW; i++)
		writel(rst_val, kbc->mmio + KBC_ROW0_MASK_0 + i * 4);

	if (filter) {
		for (i = 0; i < pdata->wake_cnt; i++) {
			u32 val, addr;
			addr = pdata->wake_cfg[i].row * 4 + KBC_ROW0_MASK_0;
			val = readl(kbc->mmio + addr);
			val &= ~(1 << pdata->wake_cfg[i].col);
			writel(val, kbc->mmio + addr);
		}
	}
}

static void tegra_kbc_config_pins(struct tegra_kbc *kbc)
{
	const struct tegra_kbc_platform_data *pdata = kbc->pdata;
	int i;

	for (i = 0; i < KBC_MAX_GPIO; i++) {
		u32 r_shft = 5 * (i % 6);
		u32 c_shft = 4 * (i % 8);
		u32 r_mask = 0x1f << r_shft;
		u32 c_mask = 0x0f << c_shft;
		u32 r_offs = (i / 6) * 4 + KBC_ROW_CFG0_0;
		u32 c_offs = (i / 8) * 4 + KBC_COL_CFG0_0;
		u32 row_cfg = readl(kbc->mmio + r_offs);
		u32 col_cfg = readl(kbc->mmio + c_offs);

		row_cfg &= ~r_mask;
		col_cfg &= ~c_mask;

		switch (pdata->pin_cfg[i].type) {
		case PIN_CFG_ROW:
			row_cfg |= ((pdata->pin_cfg[i].num << 1) | 1) << r_shft;
			break;

		case PIN_CFG_COL:
			col_cfg |= ((pdata->pin_cfg[i].num << 1) | 1) << c_shft;
			break;

		case PIN_CFG_IGNORE:
			break;
		}

		writel(row_cfg, kbc->mmio + r_offs);
		writel(col_cfg, kbc->mmio + c_offs);
	}
}

static int tegra_kbc_start(struct tegra_kbc *kbc)
{
	const struct tegra_kbc_platform_data *pdata = kbc->pdata;
	unsigned int debounce_cnt;
	u32 val = 0;

	clk_prepare_enable(kbc->clk);
	kbc->is_open = 1;

	/* Reset the KBC controller to clear all previous status.*/
	tegra_periph_reset_assert(kbc->clk);
	udelay(100);
	tegra_periph_reset_deassert(kbc->clk);
	udelay(100);

	tegra_kbc_config_pins(kbc);
	tegra_kbc_setup_wakekeys(kbc, false);

	writel(pdata->repeat_cnt, kbc->mmio + KBC_RPT_DLY_0);

	/* Keyboard debounce count is maximum of 12 bits. */
	debounce_cnt = min(pdata->debounce_cnt, KBC_MAX_DEBOUNCE_CNT);
	val = KBC_DEBOUNCE_CNT_SHIFT(debounce_cnt);
	val |= KBC_FIFO_TH_CNT_SHIFT(1); /* set fifo interrupt threshold to 1 */
	val |= KBC_CONTROL_FIFO_CNT_INT_EN;  /* interrupt on FIFO threshold */
	val |= KBC_CONTROL_KBC_EN;     /* enable */
	val |= KBC_CONTROL_KP_INT_EN;
	writel(val, kbc->mmio + KBC_CONTROL_0);

	writel(DEFAULT_INIT_DLY, kbc->mmio + KBC_INIT_DLY_0);
	writel(kbc->scan_timeout_count, kbc->mmio + KBC_TO_CNT_0);

	/*
	 * Compute the delay(ns) from interrupt mode to continuous polling
	 * mode so the timer routine is scheduled appropriately.
	 */
	val = readl(kbc->mmio + KBC_INIT_DLY_0);
	kbc->cp_dly_jiffies = usecs_to_jiffies((val & 0xfffff) * 32);

	kbc->num_pressed_keys = 0;

	/*
	 * Atomically clear out any remaining entries in the key FIFO
	 * and enable keyboard interrupts.
	 */
	while (1) {
		val = readl(kbc->mmio + KBC_INT_0);
		val >>= 4;
		if (!val)
			break;

		val = readl(kbc->mmio + KBC_KP_ENT0_0);
		val = readl(kbc->mmio + KBC_KP_ENT1_0);
	}
	writel(0x7, kbc->mmio + KBC_INT_0);

	enable_irq(kbc->irq);

	return 0;
}

static void tegra_kbc_stop(struct tegra_kbc *kbc)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&kbc->lock, flags);
	val = readl(kbc->mmio + KBC_CONTROL_0);
	val &= ~1;
	writel(val, kbc->mmio + KBC_CONTROL_0);
	spin_unlock_irqrestore(&kbc->lock, flags);

	disable_irq(kbc->irq);
	del_timer_sync(&kbc->timer);

	clk_disable_unprepare(kbc->clk);
	kbc->is_open = 0;
}

static int tegra_kbc_open(struct input_dev *dev)
{
	struct tegra_kbc *kbc = input_get_drvdata(dev);

	return tegra_kbc_start(kbc);
}

static void tegra_kbc_close(struct input_dev *dev)
{
	struct tegra_kbc *kbc = input_get_drvdata(dev);

	return tegra_kbc_stop(kbc);
}

static bool
tegra_kbc_check_pin_cfg(const struct tegra_kbc_platform_data *pdata,
			struct device *dev, unsigned int *num_rows)
{
	int i;

	*num_rows = 0;

	for (i = 0; i < KBC_MAX_GPIO; i++) {
		const struct tegra_kbc_pin_cfg *pin_cfg = &pdata->pin_cfg[i];

		switch (pin_cfg->type) {
		case PIN_CFG_ROW:
			if (pin_cfg->num >= KBC_MAX_ROW) {
				dev_err(dev,
					"pin_cfg[%d]: invalid row number %d\n",
					i, pin_cfg->num);
				return false;
			}
			(*num_rows)++;
			break;

		case PIN_CFG_COL:
			if (pin_cfg->num >= KBC_MAX_COL) {
				dev_err(dev,
					"pin_cfg[%d]: invalid column number %d\n",
					i, pin_cfg->num);
				return false;
			}
			break;

		case PIN_CFG_IGNORE:
			break;

		default:
			dev_err(dev,
				"pin_cfg[%d]: invalid entry type %d\n",
				pin_cfg->type, pin_cfg->num);
			return false;
		}
	}

	return true;
}

#ifdef CONFIG_OF
static struct tegra_kbc_platform_data *tegra_kbc_dt_parse_pdata(
	struct platform_device *pdev)
{
	struct tegra_kbc_platform_data *pdata;
	struct device_node *np = pdev->dev.of_node;
	u32 prop;
	int i;
	u32 num_rows = 0;
	u32 num_cols = 0;
	u32 cols_cfg[KBC_MAX_GPIO];
	u32 rows_cfg[KBC_MAX_GPIO];
	int proplen;
	int ret;

	if (!np) {
		dev_err(&pdev->dev, "device tree data is missing\n");
		return ERR_PTR(-ENOENT);
	}

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	if (!of_property_read_u32(np, "nvidia,debounce-delay-ms", &prop))
		pdata->debounce_cnt = prop;

	if (!of_property_read_u32(np, "nvidia,repeat-delay-ms", &prop))
		pdata->repeat_cnt = prop;

	if (of_find_property(np, "nvidia,ghost-filter", NULL))
		pdata->use_ghost_filter = true;

	if (of_find_property(np, "nvidia,wakeup-source", NULL))
		pdata->wakeup = true;

	if (!of_get_property(np, "nvidia,kbc-row-pins", &proplen)) {
		dev_err(&pdev->dev, "property nvidia,kbc-row-pins not found\n");
		return ERR_PTR(-ENOENT);
	}
	num_rows = proplen / sizeof(u32);

	if (!of_get_property(np, "nvidia,kbc-col-pins", &proplen)) {
		dev_err(&pdev->dev, "property nvidia,kbc-col-pins not found\n");
		return ERR_PTR(-ENOENT);
	}
	num_cols = proplen / sizeof(u32);

	if (!of_get_property(np, "linux,keymap", &proplen)) {
		dev_err(&pdev->dev, "property linux,keymap not found\n");
		return ERR_PTR(-ENOENT);
	}

	if (!num_rows || !num_cols || ((num_rows + num_cols) > KBC_MAX_GPIO)) {
		dev_err(&pdev->dev,
			"keypad rows/columns not porperly specified\n");
		return ERR_PTR(-EINVAL);
	}

	/* Set all pins as non-configured */
	for (i = 0; i < KBC_MAX_GPIO; i++)
		pdata->pin_cfg[i].type = PIN_CFG_IGNORE;

	ret = of_property_read_u32_array(np, "nvidia,kbc-row-pins",
				rows_cfg, num_rows);
	if (ret < 0) {
		dev_err(&pdev->dev, "Rows configurations are not proper\n");
		return ERR_PTR(-EINVAL);
	}

	ret = of_property_read_u32_array(np, "nvidia,kbc-col-pins",
				cols_cfg, num_cols);
	if (ret < 0) {
		dev_err(&pdev->dev, "Cols configurations are not proper\n");
		return ERR_PTR(-EINVAL);
	}

	for (i = 0; i < num_rows; i++) {
		pdata->pin_cfg[rows_cfg[i]].type = PIN_CFG_ROW;
		pdata->pin_cfg[rows_cfg[i]].num = i;
	}

	for (i = 0; i < num_cols; i++) {
		pdata->pin_cfg[cols_cfg[i]].type = PIN_CFG_COL;
		pdata->pin_cfg[cols_cfg[i]].num = i;
	}

	return pdata;
}
#else
static inline struct tegra_kbc_platform_data *tegra_kbc_dt_parse_pdata(
	struct platform_device *pdev)
{
	dev_err(&pdev->dev, "platform data is missing\n");
	return ERR_PTR(-EINVAL);
}
#endif

static int tegra_kbc_probe(struct platform_device *pdev)
{
	const struct tegra_kbc_platform_data *pdata = pdev->dev.platform_data;
	struct tegra_kbc *kbc;
	struct input_dev *input_dev;
	struct resource *res;
	int irq;
	int err;
	int i;
	int num_rows = 0;
	unsigned int debounce_cnt;
	unsigned int scan_time_rows;
	unsigned long scan_tc;
	unsigned int keymap_rows = KBC_MAX_KEY;

	if (!pdata)
		pdata = tegra_kbc_dt_parse_pdata(pdev);

	if (IS_ERR(pdata))
		return PTR_ERR(pdata);

	if (!tegra_kbc_check_pin_cfg(pdata, &pdev->dev, &num_rows))
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get I/O memory\n");
		return -ENXIO;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get keyboard IRQ\n");
		return -ENXIO;
	}

	kbc = devm_kzalloc(&pdev->dev, sizeof(*kbc), GFP_KERNEL);
	if (!kbc) {
		dev_err(&pdev->dev, "failed to alloc memory for kbc\n");
		return -ENOMEM;
	}

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	kbc->pdata = pdata;
	kbc->idev = input_dev;
	kbc->irq = irq;
	spin_lock_init(&kbc->lock);
	setup_timer(&kbc->timer, tegra_kbc_keypress_timer, (unsigned long)kbc);

	kbc->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(kbc->mmio))
		return PTR_ERR(kbc->mmio);

	kbc->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(kbc->clk)) {
		dev_err(&pdev->dev, "failed to get keyboard clock\n");
		return PTR_ERR(kbc->clk);
	}

	kbc->is_open = 0;
	kbc->wake_enable_rows = 0;
	kbc->wake_enable_cols = 0;
	for (i = 0; i < pdata->wake_cnt; i++) {
		kbc->wake_enable_rows |= (1 << pdata->wake_cfg[i].row);
		kbc->wake_enable_cols |= (1 << pdata->wake_cfg[i].col);
	}

	/*
	 * The time delay between two consecutive reads of the FIFO is
	 * the sum of the repeat time and the time taken for scanning
	 * the rows. There is an additional delay before the row scanning
	 * starts. The repoll delay is computed in milliseconds.
	 */
	debounce_cnt = min(pdata->debounce_cnt, KBC_MAX_DEBOUNCE_CNT);
	scan_time_rows = (KBC_ROW_SCAN_TIME + debounce_cnt) * num_rows;
	kbc->repoll_dly = KBC_ROW_SCAN_DLY + scan_time_rows + pdata->repeat_cnt;
	kbc->repoll_dly = DIV_ROUND_UP(kbc->repoll_dly, KBC_CYCLE_MS);

	if (pdata->scan_count)
		scan_tc = DEFAULT_INIT_DLY + (scan_time_rows +
				pdata->repeat_cnt) * pdata->scan_count;
	else
		scan_tc = DEFAULT_INIT_DLY + (scan_time_rows +
				pdata->repeat_cnt) * DEFAULT_SCAN_COUNT;

	kbc->one_scan_time = scan_time_rows + pdata->repeat_cnt;
	/* Bit 19:0 is for scan timeout count */
	kbc->scan_timeout_count = scan_tc & 0xFFFFF;

	kbc->wakeup_key = pdata->wakeup_key;
	kbc->use_fn_map = pdata->use_fn_map;
	kbc->use_ghost_filter = pdata->use_ghost_filter;

	input_dev->name = pdev->name;
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &pdev->dev;
	input_dev->open = tegra_kbc_open;
	input_dev->close = tegra_kbc_close;

	if (pdata->keymap_data && pdata->use_fn_map)
		keymap_rows *= 2;

	err = matrix_keypad_build_keymap(pdata->keymap_data, NULL,
					 keymap_rows, KBC_MAX_COL,
					 kbc->keycode, input_dev);
	if (err) {
		dev_err(&pdev->dev, "failed to setup keymap\n");
		return err;
	}

	if (!pdata->disable_ev_rep)
		__set_bit(EV_REP, input_dev->evbit);

	input_set_capability(input_dev, EV_MSC, MSC_SCAN);

	input_set_drvdata(input_dev, kbc);

	err = devm_request_irq(&pdev->dev, kbc->irq, tegra_kbc_isr,
			  IRQF_NO_SUSPEND | IRQF_TRIGGER_HIGH, pdev->name, kbc);
	if (err) {
		dev_err(&pdev->dev, "failed to request keyboard IRQ\n");
		return err;
	}

	disable_irq(kbc->irq);

	err = input_register_device(kbc->idev);
	if (err) {
		dev_err(&pdev->dev, "failed to register input device\n");
		return err;
	}

	platform_set_drvdata(pdev, kbc);
	device_init_wakeup(&pdev->dev, pdata->wakeup);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static void tegra_kbc_set_keypress_interrupt(struct tegra_kbc *kbc, bool enable)
{
	u32 val;

	val = readl(kbc->mmio + KBC_CONTROL_0);
	if (enable)
		val |= KBC_CONTROL_KEYPRESS_INT_EN;
	else
		val &= ~KBC_CONTROL_KEYPRESS_INT_EN;
	writel(val, kbc->mmio + KBC_CONTROL_0);
}

static int tegra_kbc_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_kbc *kbc = platform_get_drvdata(pdev);

	if (!kbc->is_open)
		return 0;

	mutex_lock(&kbc->idev->mutex);
	if (device_may_wakeup(&pdev->dev)) {
		disable_irq(kbc->irq);
		del_timer_sync(&kbc->timer);
		tegra_kbc_set_fifo_interrupt(kbc, false);

		/* Forcefully clear the interrupt status */
		writel(0x7, kbc->mmio + KBC_INT_0);
		/*
		 * Store the previous resident time of continuous polling mode.
		 * Force the keyboard into interrupt mode.
		 */
		kbc->cp_to_wkup_dly = readl(kbc->mmio + KBC_TO_CNT_0);
		writel(0, kbc->mmio + KBC_TO_CNT_0);

		tegra_kbc_setup_wakekeys(kbc, true);
		msleep(30);

		kbc->keypress_caused_wake = false;
		/* Enable keypress interrupt before going into suspend. */
		tegra_kbc_set_keypress_interrupt(kbc, true);
		enable_irq(kbc->irq);
		enable_irq_wake(kbc->irq);
	} else {
		if (kbc->idev->users)
			tegra_kbc_stop(kbc);
	}
	mutex_unlock(&kbc->idev->mutex);

	return 0;
}

static int tegra_kbc_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_kbc *kbc = platform_get_drvdata(pdev);
	int err = 0;

	if (!kbc->is_open)
		return tegra_kbc_start(kbc);

	mutex_lock(&kbc->idev->mutex);
	if (device_may_wakeup(&pdev->dev)) {
		disable_irq_wake(kbc->irq);
		tegra_kbc_setup_wakekeys(kbc, false);
		/* We will use fifo interrupts for key detection. */
		tegra_kbc_set_keypress_interrupt(kbc, false);

		/* Restore the resident time of continuous polling mode. */
		writel(kbc->cp_to_wkup_dly, kbc->mmio + KBC_TO_CNT_0);

		tegra_kbc_set_fifo_interrupt(kbc, true);

		if (kbc->keypress_caused_wake && kbc->wakeup_key) {
			/*
			 * We can't report events directly from the ISR
			 * because timekeeping is stopped when processing
			 * wakeup request and we get a nasty warning when
			 * we try to call do_gettimeofday() in evdev
			 * handler.
			 */
			input_report_key(kbc->idev, kbc->wakeup_key, 1);
			input_report_key(kbc->idev, kbc->wakeup_key, 0);
			input_sync(kbc->idev);
		}
	} else {
		if (kbc->idev->users)
			err = tegra_kbc_start(kbc);
	}
	mutex_unlock(&kbc->idev->mutex);

	return err;
}
#endif

static SIMPLE_DEV_PM_OPS(tegra_kbc_pm_ops, tegra_kbc_suspend, tegra_kbc_resume);

static const struct of_device_id tegra_kbc_of_match[] = {
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	{ .compatible = "nvidia,tegra20-kbc", },
#endif
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	{ .compatible = "nvidia,tegra30-kbc", },
#endif
#ifdef CONFIG_ARCH_TEGRA_11x_SOC
	{ .compatible = "nvidia,tegra114-kbc", },
#endif
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_kbc_of_match);

static struct platform_driver tegra_kbc_driver = {
	.probe		= tegra_kbc_probe,
	.driver	= {
		.name	= "tegra-kbc",
		.owner  = THIS_MODULE,
		.pm	= &tegra_kbc_pm_ops,
		.of_match_table = tegra_kbc_of_match,
	},
};
module_platform_driver(tegra_kbc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rakesh Iyer <riyer@nvidia.com>");
MODULE_DESCRIPTION("Tegra matrix keyboard controller driver");
MODULE_ALIAS("platform:tegra-kbc");
