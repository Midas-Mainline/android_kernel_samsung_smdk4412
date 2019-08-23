#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/platform_device.h>


struct regdev {
	struct device *dev;
	struct clk *clk;
	struct regulator *reg;
};

static const struct of_device_id init_device_ids[] = {
	{ .compatible = "stupid,regulator-loader" },
	{},
};

static int reg_probe(struct platform_device *pdev)
{
	struct regdev *dev;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;

	dev->clk = devm_clk_get(dev->dev, NULL);
	if (IS_ERR(dev->clk)) {
		dev_err(dev->dev, "Clock error: %d\n", PTR_ERR(dev->clk));
		if (PTR_ERR(dev->clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev->clk = NULL;
	} else
		clk_prepare_enable(dev->clk);

	dev->reg = devm_regulator_get(dev->dev, "default");
	if (IS_ERR(dev->reg))
		dev->reg = NULL;
	else
		if (regulator_enable(dev->reg) < 0) dev_err(dev->dev, "regulator_enable failed\n");

	platform_set_drvdata(pdev, dev);

	dev_err(dev->dev, "Loaded regulator + clock driver, clk=%p, reg=%p\n", dev->clk, dev->reg);
	return 0;
}

static int reg_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%p unloading\n", pdev);
	return 0;
}

static struct platform_driver reg_driver = {
	.driver = {
		.name = "regulator-loader",
		.of_match_table = init_device_ids,
	},
	.probe = reg_probe,
	.remove = reg_remove,
};

static int __init reg_init(void)
{
	return platform_driver_register(&reg_driver);
}

device_initcall(reg_init);
