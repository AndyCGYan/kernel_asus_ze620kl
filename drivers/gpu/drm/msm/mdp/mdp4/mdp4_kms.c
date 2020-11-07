/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_mmu.h"
#include "mdp4_kms.h"

static struct mdp4_platform_config *mdp4_get_config(struct platform_device *dev);

static int mdp4_hw_init(struct msm_kms *kms)
{
	struct mdp4_kms *mdp4_kms = to_mdp4_kms(to_mdp_kms(kms));
	struct drm_device *dev = mdp4_kms->dev;
	uint32_t version, major, minor, dmap_cfg, vg_cfg;
	unsigned long clk;
	int ret = 0;

	pm_runtime_get_sync(dev->dev);

	mdp4_enable(mdp4_kms);
	version = mdp4_read(mdp4_kms, REG_MDP4_VERSION);
	mdp4_disable(mdp4_kms);

	major = FIELD(version, MDP4_VERSION_MAJOR);
	minor = FIELD(version, MDP4_VERSION_MINOR);

	DBG("found MDP4 version v%d.%d", major, minor);

	if (major != 4) {
		dev_err(dev->dev, "unexpected MDP version: v%d.%d\n",
				major, minor);
		ret = -ENXIO;
		goto out;
	}

	mdp4_kms->rev = minor;

	if (mdp4_kms->dsi_pll_vdda) {
		if ((mdp4_kms->rev == 2) || (mdp4_kms->rev == 4)) {
			ret = regulator_set_voltage(mdp4_kms->dsi_pll_vdda,
					1200000, 1200000);
			if (ret) {
				dev_err(dev->dev,
					"failed to set dsi_pll_vdda voltage: %d\n", ret);
				goto out;
			}
		}
	}

	if (mdp4_kms->dsi_pll_vddio) {
		if (mdp4_kms->rev == 2) {
			ret = regulator_set_voltage(mdp4_kms->dsi_pll_vddio,
					1800000, 1800000);
			if (ret) {
				dev_err(dev->dev,
					"failed to set dsi_pll_vddio voltage: %d\n", ret);
				goto out;
			}
		}
	}

	if (mdp4_kms->rev > 1) {
		mdp4_write(mdp4_kms, REG_MDP4_CS_CONTROLLER0, 0x0707ffff);
		mdp4_write(mdp4_kms, REG_MDP4_CS_CONTROLLER1, 0x03073f3f);
	}

	mdp4_write(mdp4_kms, REG_MDP4_PORTMAP_MODE, 0x3);

	/* max read pending cmd config, 3 pending requests: */
	mdp4_write(mdp4_kms, REG_MDP4_READ_CNFG, 0x02222);

	clk = clk_get_rate(mdp4_kms->clk);

	if ((mdp4_kms->rev >= 1) || (clk >= 90000000)) {
		dmap_cfg = 0x47;     /* 16 bytes-burst x 8 req */
		vg_cfg = 0x47;       /* 16 bytes-burs x 8 req */
	} else {
		dmap_cfg = 0x27;     /* 8 bytes-burst x 8 req */
		vg_cfg = 0x43;       /* 16 bytes-burst x 4 req */
	}

	DBG("fetch config: dmap=%02x, vg=%02x", dmap_cfg, vg_cfg);

	mdp4_write(mdp4_kms, REG_MDP4_DMA_FETCH_CONFIG(DMA_P), dmap_cfg);
	mdp4_write(mdp4_kms, REG_MDP4_DMA_FETCH_CONFIG(DMA_E), dmap_cfg);

	mdp4_write(mdp4_kms, REG_MDP4_PIPE_FETCH_CONFIG(VG1), vg_cfg);
	mdp4_write(mdp4_kms, REG_MDP4_PIPE_FETCH_CONFIG(VG2), vg_cfg);
	mdp4_write(mdp4_kms, REG_MDP4_PIPE_FETCH_CONFIG(RGB1), vg_cfg);
	mdp4_write(mdp4_kms, REG_MDP4_PIPE_FETCH_CONFIG(RGB2), vg_cfg);

	if (mdp4_kms->rev >= 2)
		mdp4_write(mdp4_kms, REG_MDP4_LAYERMIXER_IN_CFG_UPDATE_METHOD, 1);
	mdp4_write(mdp4_kms, REG_MDP4_LAYERMIXER_IN_CFG, 0);

	/* disable CSC matrix / YUV by default: */
	mdp4_write(mdp4_kms, REG_MDP4_PIPE_OP_MODE(VG1), 0);
	mdp4_write(mdp4_kms, REG_MDP4_PIPE_OP_MODE(VG2), 0);
	mdp4_write(mdp4_kms, REG_MDP4_DMA_P_OP_MODE, 0);
	mdp4_write(mdp4_kms, REG_MDP4_DMA_S_OP_MODE, 0);
	mdp4_write(mdp4_kms, REG_MDP4_OVLP_CSC_CONFIG(1), 0);
	mdp4_write(mdp4_kms, REG_MDP4_OVLP_CSC_CONFIG(2), 0);

	if (mdp4_kms->rev > 1)
		mdp4_write(mdp4_kms, REG_MDP4_RESET_STATUS, 1);

	dev->mode_config.allow_fb_modifiers = true;

out:
	pm_runtime_put_sync(dev->dev);

	return ret;
}

static void mdp4_prepare_commit(struct msm_kms *kms, struct drm_atomic_state *state)
{
	struct mdp4_kms *mdp4_kms = to_mdp4_kms(to_mdp_kms(kms));
	int i, ncrtcs = state->dev->mode_config.num_crtc;

	mdp4_enable(mdp4_kms);

	/* see 119ecb7fd */
	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc *crtc = state->crtcs[i];
		if (!crtc)
			continue;
		drm_crtc_vblank_get(crtc);
	}
}

static void mdp4_complete_commit(struct msm_kms *kms, struct drm_atomic_state *state)
{
	struct mdp4_kms *mdp4_kms = to_mdp4_kms(to_mdp_kms(kms));
	int i, ncrtcs = state->dev->mode_config.num_crtc;

	/* see 119ecb7fd */
	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc *crtc = state->crtcs[i];
		if (!crtc)
			continue;
		drm_crtc_vblank_put(crtc);
	}

	mdp4_disable(mdp4_kms);
}

static void mdp4_wait_for_crtc_commit_done(struct msm_kms *kms,
						struct drm_crtc *crtc)
{
	mdp4_crtc_wait_for_commit_done(crtc);
}

static long mdp4_round_pixclk(struct msm_kms *kms, unsigned long rate,
		struct drm_encoder *encoder)
{
	/* if we had >1 encoder, we'd need something more clever: */
	return mdp4_dtv_round_pixclk(encoder, rate);
}

static void mdp4_preclose(struct msm_kms *kms, struct drm_file *file)
{
	struct mdp4_kms *mdp4_kms = to_mdp4_kms(to_mdp_kms(kms));
	struct msm_drm_private *priv = mdp4_kms->dev->dev_private;
	unsigned i;
	struct msm_gem_address_space *aspace = mdp4_kms->aspace;

	for (i = 0; i < priv->num_crtcs; i++)
		mdp4_crtc_cancel_pending_flip(priv->crtcs[i], file);

	if (aspace) {
		aspace->mmu->funcs->detach(aspace->mmu);
		msm_gem_address_space_destroy(aspace);
	}
}

static void mdp4_destroy(struct msm_kms *kms)
{
	struct device *dev = mdp4_kms->dev->dev;
	struct msm_gem_address_space *aspace = mdp4_kms->aspace;

	struct mdp4_kms *mdp4_kms = to_mdp4_kms(to_mdp_kms(kms));
	if (mdp4_kms->blank_cursor_iova)
		msm_gem_put_iova(mdp4_kms->blank_cursor_bo, mdp4_kms->aspace);
	if (mdp4_kms->blank_cursor_bo)
		drm_gem_object_unreference_unlocked(mdp4_kms->blank_cursor_bo);

	if (aspace) {
		aspace->mmu->funcs->detach(aspace->mmu);
		msm_gem_address_space_put(aspace);
	}

	kfree(mdp4_kms);
}

static const struct mdp_kms_funcs kms_funcs = {
	.base = {
		.hw_init         = mdp4_hw_init,
		.irq_preinstall  = mdp4_irq_preinstall,
		.irq_postinstall = mdp4_irq_postinstall,
		.irq_uninstall   = mdp4_irq_uninstall,
		.irq             = mdp4_irq,
		.enable_vblank   = mdp4_enable_vblank,
		.disable_vblank  = mdp4_disable_vblank,
		.prepare_commit  = mdp4_prepare_commit,
		.complete_commit = mdp4_complete_commit,
		.wait_for_crtc_commit_done = mdp4_wait_for_crtc_commit_done,
		.get_format      = mdp_get_format,
		.round_pixclk    = mdp4_round_pixclk,
		.preclose        = mdp4_preclose,
		.destroy         = mdp4_destroy,
	},
	.set_irqmask         = mdp4_set_irqmask,
};

int mdp4_disable(struct mdp4_kms *mdp4_kms)
{
	DBG("");

	clk_disable_unprepare(mdp4_kms->clk);
	if (mdp4_kms->pclk)
		clk_disable_unprepare(mdp4_kms->pclk);
	clk_disable_unprepare(mdp4_kms->lut_clk);
	if (mdp4_kms->axi_clk)
		clk_disable_unprepare(mdp4_kms->axi_clk);

	return 0;
}

int mdp4_enable(struct mdp4_kms *mdp4_kms)
{
	DBG("");

	clk_prepare_enable(mdp4_kms->clk);
	if (mdp4_kms->pclk)
		clk_prepare_enable(mdp4_kms->pclk);
	clk_prepare_enable(mdp4_kms->lut_clk);
	if (mdp4_kms->axi_clk)
		clk_prepare_enable(mdp4_kms->axi_clk);

	return 0;
}

#ifdef CONFIG_OF
static struct drm_panel *detect_panel(struct drm_device *dev)
{
	struct device_node *endpoint, *panel_node;
	struct device_node *np = dev->dev->of_node;
	struct drm_panel *panel = NULL;

	endpoint = of_graph_get_next_endpoint(np, NULL);
	if (!endpoint) {
		dev_err(dev->dev, "no valid endpoint\n");
		return ERR_PTR(-ENODEV);
	}

	panel_node = of_graph_get_remote_port_parent(endpoint);
	if (!panel_node) {
		dev_err(dev->dev, "no valid panel node\n");
		of_node_put(endpoint);
		return ERR_PTR(-ENODEV);
	}

	of_node_put(endpoint);

	panel = of_drm_find_panel(panel_node);
	if (!panel) {
		of_node_put(panel_node);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return panel;
}
#else
static struct drm_panel *detect_panel(struct drm_device *dev)
{
	// ??? maybe use a module param to specify which panel is attached?
}
#endif

static int modeset_init(struct mdp4_kms *mdp4_kms)
{
	struct drm_device *dev = mdp4_kms->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	struct drm_panel *panel;
	int ret;

	/* construct non-private planes: */
	plane = mdp4_plane_init(dev, VG1, false);
	if (IS_ERR(plane)) {
		dev_err(dev->dev, "failed to construct plane for VG1\n");
		ret = PTR_ERR(plane);
		goto fail;
	}
	priv->planes[priv->num_planes++] = plane;

	plane = mdp4_plane_init(dev, VG2, false);
	if (IS_ERR(plane)) {
		dev_err(dev->dev, "failed to construct plane for VG2\n");
		ret = PTR_ERR(plane);
		goto fail;
	}
	priv->planes[priv->num_planes++] = plane;

	/*
	 * Setup the LCDC/LVDS path: RGB2 -> DMA_P -> LCDC -> LVDS:
	 */

	panel = detect_panel(dev);
	if (IS_ERR(panel)) {
		ret = PTR_ERR(panel);
		dev_err(dev->dev, "failed to detect LVDS panel: %d\n", ret);
		goto fail;
	}

	plane = mdp4_plane_init(dev, RGB2, true);
	if (IS_ERR(plane)) {
		dev_err(dev->dev, "failed to construct plane for RGB2\n");
		ret = PTR_ERR(plane);
		goto fail;
	}

	crtc  = mdp4_crtc_init(dev, plane, priv->num_crtcs, 0, DMA_P);
	if (IS_ERR(crtc)) {
		dev_err(dev->dev, "failed to construct crtc for DMA_P\n");
		ret = PTR_ERR(crtc);
		goto fail;
	}

	encoder = mdp4_lcdc_encoder_init(dev, panel);
	if (IS_ERR(encoder)) {
		dev_err(dev->dev, "failed to construct LCDC encoder\n");
		ret = PTR_ERR(encoder);
		goto fail;
	}

	/* LCDC can be hooked to DMA_P: */
	encoder->possible_crtcs = 1 << priv->num_crtcs;

	priv->crtcs[priv->num_crtcs++] = crtc;
	priv->encoders[priv->num_encoders++] = encoder;

	connector = mdp4_lvds_connector_init(dev, panel, encoder);
	if (IS_ERR(connector)) {
		ret = PTR_ERR(connector);
		dev_err(dev->dev, "failed to initialize LVDS connector: %d\n", ret);
		goto fail;
	}

	priv->connectors[priv->num_connectors++] = connector;

	/*
	 * Setup DTV/HDMI path: RGB1 -> DMA_E -> DTV -> HDMI:
	 */

	plane = mdp4_plane_init(dev, RGB1, true);
	if (IS_ERR(plane)) {
		dev_err(dev->dev, "failed to construct plane for RGB1\n");
		ret = PTR_ERR(plane);
		goto fail;
	}

	crtc  = mdp4_crtc_init(dev, plane, priv->num_crtcs, 1, DMA_E);
	if (IS_ERR(crtc)) {
		dev_err(dev->dev, "failed to construct crtc for DMA_E\n");
		ret = PTR_ERR(crtc);
		goto fail;
	}

	encoder = mdp4_dtv_encoder_init(dev);
	if (IS_ERR(encoder)) {
		dev_err(dev->dev, "failed to construct DTV encoder\n");
		ret = PTR_ERR(encoder);
		goto fail;
	}

	/* DTV can be hooked to DMA_E: */
	encoder->possible_crtcs = 1 << priv->num_crtcs;

	priv->crtcs[priv->num_crtcs++] = crtc;
	priv->encoders[priv->num_encoders++] = encoder;

	if (priv->hdmi) {
		/* Construct bridge/connector for HDMI: */
		ret = hdmi_modeset_init(priv->hdmi, dev, encoder);
		if (ret) {
			dev_err(dev->dev, "failed to initialize HDMI: %d\n", ret);
			goto fail;
		}
	}

	return 0;

fail:
	return ret;
}

struct msm_kms *mdp4_kms_init(struct drm_device *dev)
{
	struct platform_device *pdev = dev->platformdev;
	struct mdp4_platform_config *config = mdp4_get_config(pdev);
	struct mdp4_kms *mdp4_kms;
	struct msm_kms *kms = NULL;
	struct msm_gem_address_space *aspace;
	int ret;

	mdp4_kms = kzalloc(sizeof(*mdp4_kms), GFP_KERNEL);
	if (!mdp4_kms) {
		dev_err(dev->dev, "failed to allocate kms\n");
		ret = -ENOMEM;
		goto fail;
	}

	mdp_kms_init(&mdp4_kms->base, &kms_funcs);

	kms = &mdp4_kms->base.base;

	mdp4_kms->dev = dev;

	mdp4_kms->mmio = msm_ioremap(pdev, NULL, "MDP4");
	if (IS_ERR(mdp4_kms->mmio)) {
		ret = PTR_ERR(mdp4_kms->mmio);
		goto fail;
	}

	mdp4_kms->dsi_pll_vdda =
			devm_regulator_get_optional(&pdev->dev, "dsi_pll_vdda");
	if (IS_ERR(mdp4_kms->dsi_pll_vdda))
		mdp4_kms->dsi_pll_vdda = NULL;

	mdp4_kms->dsi_pll_vddio =
			devm_regulator_get_optional(&pdev->dev, "dsi_pll_vddio");
	if (IS_ERR(mdp4_kms->dsi_pll_vddio))
		mdp4_kms->dsi_pll_vddio = NULL;

	/* NOTE: driver for this regulator still missing upstream.. use
	 * _get_exclusive() and ignore the error if it does not exist
	 * (and hope that the bootloader left it on for us)
	 */
	mdp4_kms->vdd = devm_regulator_get_exclusive(&pdev->dev, "vdd");
	if (IS_ERR(mdp4_kms->vdd))
		mdp4_kms->vdd = NULL;

	if (mdp4_kms->vdd) {
		ret = regulator_enable(mdp4_kms->vdd);
		if (ret) {
			dev_err(dev->dev, "failed to enable regulator vdd: %d\n", ret);
			goto fail;
		}
	}

	mdp4_kms->clk = devm_clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(mdp4_kms->clk)) {
		dev_err(dev->dev, "failed to get core_clk\n");
		ret = PTR_ERR(mdp4_kms->clk);
		goto fail;
	}

	mdp4_kms->pclk = devm_clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(mdp4_kms->pclk))
		mdp4_kms->pclk = NULL;

	// XXX if (rev >= MDP_REV_42) { ???
	mdp4_kms->lut_clk = devm_clk_get(&pdev->dev, "lut_clk");
	if (IS_ERR(mdp4_kms->lut_clk)) {
		dev_err(dev->dev, "failed to get lut_clk\n");
		ret = PTR_ERR(mdp4_kms->lut_clk);
		goto fail;
	}

	mdp4_kms->axi_clk = devm_clk_get(&pdev->dev, "mdp_axi_clk");
	if (IS_ERR(mdp4_kms->axi_clk)) {
		dev_err(dev->dev, "failed to get axi_clk\n");
		ret = PTR_ERR(mdp4_kms->axi_clk);
		goto fail;
	}

	clk_set_rate(mdp4_kms->clk, config->max_clk);
	clk_set_rate(mdp4_kms->lut_clk, config->max_clk);

	/* make sure things are off before attaching iommu (bootloader could
	 * have left things on, in which case we'll start getting faults if
	 * we don't disable):
	 */
	mdp4_enable(mdp4_kms);
	mdp4_write(mdp4_kms, REG_MDP4_DTV_ENABLE, 0);
	mdp4_write(mdp4_kms, REG_MDP4_LCDC_ENABLE, 0);
	mdp4_write(mdp4_kms, REG_MDP4_DSI_ENABLE, 0);
	mdp4_disable(mdp4_kms);
	mdelay(16);

	if (config->iommu) {
		config->iommu->geometry.aperture_start = 0x1000;
		config->iommu->geometry.aperture_end = 0xffffffff;

		aspace = msm_gem_address_space_create(&pdev->dev,
			config->iommu, MSM_IOMMU_DOMAIN_DEFAULT, "mdp4");
		if (IS_ERR(aspace)) {
			ret = PTR_ERR(aspace);
			goto fail;
		}

		mdp4_kms->aspace = aspace;

		ret = aspace->mmu->funcs->attach(aspace->mmu, NULL, 0);
		if (ret)
			goto fail;
	} else {
		dev_info(dev->dev, "no iommu, fallback to phys "
				"contig buffers for scanout\n");
		aspace = NULL;
	}

	ret = modeset_init(mdp4_kms);
	if (ret) {
		dev_err(dev->dev, "modeset_init failed: %d\n", ret);
		goto fail;
	}

	mdp4_kms->blank_cursor_bo = msm_gem_new(dev, SZ_16K, MSM_BO_WC);
	if (IS_ERR(mdp4_kms->blank_cursor_bo)) {
		ret = PTR_ERR(mdp4_kms->blank_cursor_bo);
		dev_err(dev->dev, "could not allocate blank-cursor bo: %d\n", ret);
		mdp4_kms->blank_cursor_bo = NULL;
		goto fail;
	}

	ret = msm_gem_get_iova(mdp4_kms->blank_cursor_bo, mdp4_kms->aspace,
			&mdp4_kms->blank_cursor_iova);
	if (ret) {
		dev_err(dev->dev, "could not pin blank-cursor bo: %d\n", ret);
		goto fail;
	}

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;

	return kms;

fail:
	if (kms)
		mdp4_destroy(kms);
	return ERR_PTR(ret);
}

static struct mdp4_platform_config *mdp4_get_config(struct platform_device *dev)
{
	static struct mdp4_platform_config config = {};
#ifdef CONFIG_OF
	/* TODO */
	config.max_clk = 266667000;
	config.iommu = iommu_domain_alloc(msm_iommu_get_bus(&dev->dev));

#else
	if (cpu_is_apq8064())
		config.max_clk = 266667000;
	else
		config.max_clk = 200000000;

	config.iommu = msm_get_iommu_domain(DISPLAY_READ_DOMAIN);
#endif
	return &config;
}
