// Vulkan 1.1 backend (volk loader, classic render passes, one frame in flight)
#include "video_backend.h"

#include "volk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "spv_shaders.h"

namespace {

struct Params { float a[4]; float b[4]; };

#define VKCHECK(expr, msg)                                                          \
	do {                                                                            \
		VkResult r_ = (expr);                                                       \
		if (r_ != VK_SUCCESS) { err = std::string(msg) + " (VkResult " + std::to_string(int(r_)) + ")"; return false; } \
	} while (0)

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t *map = nullptr; size_t size = 0; };

class VkBackend final : public IVideoBackend
{
public:
	const char *name() const override { return "Vulkan"; }

	bool init(HWND hwnd, const VideoOptions &opt, std::string &err) override
	{
		m_hwnd = hwnd;
		m_opt = opt;
		m_opt.scale = std::clamp(m_opt.scale, 1, 8);
		VKCHECK(volkInitialize(), "Vulkan loader (vulkan-1.dll) not found");

		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "Cruis'n USA PC";
		app.apiVersion = VK_API_VERSION_1_1;
		const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = iext;
		VKCHECK(vkCreateInstance(&ici, nullptr, &m_inst), "vkCreateInstance");
		volkLoadInstance(m_inst);

		VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
		sci.hinstance = GetModuleHandle(nullptr);
		sci.hwnd = hwnd;
		VKCHECK(vkCreateWin32SurfaceKHR(m_inst, &sci, nullptr, &m_surf), "vkCreateWin32SurfaceKHR");

		// physical device: prefer discrete GPUs with a graphics+present queue
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(m_inst, &n, nullptr);
		std::vector<VkPhysicalDevice> devs(n);
		vkEnumeratePhysicalDevices(m_inst, &n, devs.data());
		int best = -1;
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t qn = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
			std::vector<VkQueueFamilyProperties> qp(qn);
			vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qp.data());
			for (uint32_t f = 0; f < qn; f++)
			{
				VkBool32 present = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, m_surf, &present);
				if ((qp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
				{
					VkPhysicalDeviceProperties pp;
					vkGetPhysicalDeviceProperties(devs[i], &pp);
					int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
					if (best < 0 || score > m_score)
					{
						best = int(i); m_score = score; m_qfam = f; m_pd = devs[i];
						std::snprintf(m_devname, sizeof(m_devname), "%s", pp.deviceName);
						m_limits = pp.limits;
					}
					break;
				}
			}
		}
		if (best < 0) { err = "no Vulkan device with graphics+present support"; return false; }
		vkGetPhysicalDeviceMemoryProperties(m_pd, &m_memprops);

		float prio = 1.0f;
		VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = m_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
		const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
		VKCHECK(vkCreateDevice(m_pd, &dci, nullptr, &m_dev), "vkCreateDevice");
		volkLoadDevice(m_dev);
		vkGetDeviceQueue(m_dev, m_qfam, 0, &m_q);

		if (m_limits.maxImageDimension2D < 16384) { err = "maxImageDimension2D < 16384"; return false; }

		VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpi.queueFamilyIndex = m_qfam;
		VKCHECK(vkCreateCommandPool(m_dev, &cpi, nullptr, &m_pool), "vkCreateCommandPool");
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = m_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
		VKCHECK(vkAllocateCommandBuffers(m_dev, &cai, &m_cb), "vkAllocateCommandBuffers");
		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VKCHECK(vkCreateFence(m_dev, &fi, nullptr, &m_fence), "vkCreateFence");
		VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VKCHECK(vkCreateSemaphore(m_dev, &si, nullptr, &m_sem_acq), "semaphore");
		VKCHECK(vkCreateSemaphore(m_dev, &si, nullptr, &m_sem_rel), "semaphore");

		if (!create_buffers(err)) return false;
		if (!create_static_images(err)) return false;
		if (!create_samplers(err)) return false;
		if (!create_render_passes(err)) return false;
		if (!create_descriptors(err)) return false;
		if (!create_pipelines(err)) return false;
		if (!create_pages(err)) return false;
		RECT r; GetClientRect(hwnd, &r);
		m_win_w = std::max<int>(r.right, 1); m_win_h = std::max<int>(r.bottom, 1);
		if (!create_swapchain(err)) return false;
		return true;
	}

	void shutdown() override
	{
		if (!m_dev) return;
		vkDeviceWaitIdle(m_dev);
		destroy_swapchain();
		destroy_pages();
		for (Img *i : {&m_tex_ram, &m_tex_pal, &m_tex_ovl}) destroy_img(*i);
		for (Buf *b : {&m_instbuf, &m_stage, &m_ubo}) destroy_buf(*b);
		vkDestroySampler(m_dev, m_samp_nearest, nullptr);
		vkDestroySampler(m_dev, m_samp_linear, nullptr);
		vkDestroyPipeline(m_dev, m_pipe_quad, nullptr);
		vkDestroyPipeline(m_dev, m_pipe_present, nullptr);
		vkDestroyPipeline(m_dev, m_pipe_ovl, nullptr);
		vkDestroyPipelineLayout(m_dev, m_pl, nullptr);
		vkDestroyDescriptorPool(m_dev, m_dpool, nullptr);
		vkDestroyDescriptorSetLayout(m_dev, m_dsl, nullptr);
		vkDestroyRenderPass(m_dev, m_rp_page, nullptr);
		vkDestroyRenderPass(m_dev, m_rp_swap, nullptr);
		vkDestroySemaphore(m_dev, m_sem_acq, nullptr);
		vkDestroySemaphore(m_dev, m_sem_rel, nullptr);
		vkDestroyFence(m_dev, m_fence, nullptr);
		vkDestroyCommandPool(m_dev, m_pool, nullptr);
		vkDestroyDevice(m_dev, nullptr);
		vkDestroySurfaceKHR(m_inst, m_surf, nullptr);
		vkDestroyInstance(m_inst, nullptr);
		m_dev = VK_NULL_HANDLE;
	}

	void resize(int w, int h) override
	{
		w = std::max(w, 1); h = std::max(h, 1);
		if (w != m_win_w || h != m_win_h) { m_win_w = w; m_win_h = h; m_sc_dirty = true; }
	}

	void set_options(const VideoOptions &o) override
	{
		std::string err;
		bool rescale = std::clamp(o.scale, 1, 8) != m_opt.scale;
		bool revsync = o.vsync != m_opt.vsync;
		bool resamp = o.smooth_output != m_opt.smooth_output;
		m_opt = o;
		m_opt.scale = std::clamp(m_opt.scale, 1, 8);
		if (rescale) { vkDeviceWaitIdle(m_dev); destroy_pages(); create_pages(err); }
		if (resamp) write_present_sets();
		if (revsync) m_sc_dirty = true;
	}

	// ---- uploads -------------------------------------------------------------------------------

	void upload_palette(const uint32_t *argb, int first, int last) override
	{
		int r0 = first >> 8, r1 = last >> 8;
		copy_rows_to_image(m_tex_pal, argb + r0 * 256, 256 * 4, 256, r0, r1 - r0 + 1);
	}

	void upload_texture_rows(const uint8_t *ram, int first, int last) override
	{
		copy_rows_to_image(m_tex_ram, ram + size_t(first) * 256, 256, 256, first, last - first + 1);
	}

	void upload_overlay(int page, const uint16_t *layer, int first, int last) override
	{
		copy_rows_to_image(m_tex_ovl, layer + size_t(first) * 512, 512 * 2, 512, first, last - first + 1);
		begin_cb();
		draw_rect_pass(page, m_pipe_ovl, m_set_ovl, Params{{-1, -1, 1, 1}, {0, 0, 1, 1}});
	}

	void draw(int page, const GpuQuad *q, int count) override
	{
		if (count <= 0) return;
		begin_cb();
		const size_t stride = sizeof(GpuQuad);
		for (int done = 0; done < count;)
		{
			size_t room = (m_instbuf.size - m_instbuf_off) / stride;
			if (room < 256) { submit_wait(); begin_cb(); continue; }
			int n = int(std::min<size_t>(size_t(count - done), room));
			std::memcpy(m_instbuf.map + m_instbuf_off, q + done, size_t(n) * stride);

			Params p{{m_opt.filter_textures ? 1.0f : 0.0f, 0, 0, 0}, {0, 0, 0, 0}};
			uint32_t dyn = write_params(p);
			begin_page_pass(page);
			vkCmdBindPipeline(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_quad);
			vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pl, 0, 1, &m_set_quad, 1, &dyn);
			VkDeviceSize off = m_instbuf_off;
			vkCmdBindVertexBuffers(m_cb, 0, 1, &m_instbuf.buf, &off);
			vkCmdDraw(m_cb, 4, uint32_t(n), 0, 0);
			vkCmdEndRenderPass(m_cb);
			m_instbuf_off += size_t(n) * stride;
			done += n;
		}
	}

	void present(int page, int vis_w, int vis_h) override
	{
		if (m_sc_dirty) { std::string e; vkDeviceWaitIdle(m_dev); create_swapchain(e); }
		if (!m_sc) { submit_wait(); return; }
		begin_cb();

		uint32_t idx = 0;
		VkResult r = vkAcquireNextImageKHR(m_dev, m_sc, UINT64_MAX, m_sem_acq, VK_NULL_HANDLE, &idx);
		if (r == VK_ERROR_OUT_OF_DATE_KHR) { m_sc_dirty = true; submit_wait(); return; }
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { submit_wait(); return; }

		// letterbox rect
		float dw = float(m_sc_ext.width), dh = float(m_sc_ext.height);
		float tw = dw, th = dh;
		if (m_opt.keep_aspect)
		{
			float a = m_opt.aspect;
			if (dw / dh > a) { th = dh; tw = dh * a; } else { tw = dw; th = dw / a; }
			if (m_opt.integer_scale)
			{
				float k = std::max(1.0f, std::floor(th / float(vis_h)));
				th = k * vis_h; tw = th * a;
				if (tw > dw) { tw = dw; th = tw / a; }
			}
		}
		float x0 = (dw - tw) * 0.5f, y0 = (dh - th) * 0.5f;
		Params p{{x0 / dw * 2 - 1, y0 / dh * 2 - 1, (x0 + tw) / dw * 2 - 1, (y0 + th) / dh * 2 - 1},
		         {0, 0, float(vis_w) / 512.0f, float(vis_h) / 512.0f}};
		uint32_t dyn = write_params(p);

		VkClearValue clear{};
		VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = m_rp_swap;
		rb.framebuffer = m_sc_fb[idx];
		rb.renderArea = {{0, 0}, m_sc_ext};
		rb.clearValueCount = 1; rb.pClearValues = &clear;
		vkCmdBeginRenderPass(m_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport vp{0, 0, dw, dh, 0, 1};
		VkRect2D sc{{0, 0}, m_sc_ext};
		vkCmdSetViewport(m_cb, 0, 1, &vp);
		vkCmdSetScissor(m_cb, 0, 1, &sc);
		vkCmdBindPipeline(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_present);
		vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pl, 0, 1, &m_set_present[page & 1], 1, &dyn);
		vkCmdDraw(m_cb, 4, 1, 0, 0);
		vkCmdEndRenderPass(m_cb);
		vkEndCommandBuffer(m_cb);

		VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &m_sem_acq; sub.pWaitDstStageMask = &wait;
		sub.commandBufferCount = 1; sub.pCommandBuffers = &m_cb;
		sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &m_sem_rel;
		vkQueueSubmit(m_q, 1, &sub, m_fence);

		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &m_sem_rel;
		pi.swapchainCount = 1; pi.pSwapchains = &m_sc; pi.pImageIndices = &idx;
		VkResult pr = vkQueuePresentKHR(m_q, &pi);
		if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_sc_dirty = true;

		vkWaitForFences(m_dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_dev, 1, &m_fence);
		m_recording = false;
		m_instbuf_off = m_stage_off = m_ubo_off = 0;
	}

	bool read_page(int page, std::vector<uint32_t> &out, int &w, int &h) override
	{
		w = h = 512 * m_opt.scale;
		size_t bytes = size_t(w) * h * 4;
		Buf rb;
		std::string err;
		if (!make_buffer(rb, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, err)) return false;
		begin_cb();
		barrier(m_page[page & 1].img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy c{};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {uint32_t(w), uint32_t(h), 1};
		vkCmdCopyImageToBuffer(m_cb, m_page[page & 1].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
		barrier(m_page[page & 1].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		submit_wait();
		out.resize(size_t(w) * h);
		std::memcpy(out.data(), rb.map, bytes);
		destroy_buf(rb);
		return true;
	}

private:
	static constexpr size_t INST_BYTES = 16u << 20, STAGE_BYTES = 32u << 20, UBO_BYTES = 1u << 20;

	// ---- memory helpers ---------------------------------------------------------------------------
	uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) const
	{
		for (uint32_t i = 0; i < m_memprops.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (m_memprops.memoryTypes[i].propertyFlags & want) == want)
				return i;
		return ~0u;
	}

	bool make_buffer(Buf &b, size_t size, VkBufferUsageFlags usage, bool host, std::string &err)
	{
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VKCHECK(vkCreateBuffer(m_dev, &bi, nullptr, &b.buf), "vkCreateBuffer");
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(m_dev, b.buf, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		                                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKCHECK(vkAllocateMemory(m_dev, &ai, nullptr, &b.mem), "vkAllocateMemory(buffer)");
		vkBindBufferMemory(m_dev, b.buf, b.mem, 0);
		b.size = size;
		if (host) vkMapMemory(m_dev, b.mem, 0, size, 0, reinterpret_cast<void **>(&b.map));
		return true;
	}

	void destroy_buf(Buf &b)
	{
		if (b.map) vkUnmapMemory(m_dev, b.mem);
		if (b.buf) vkDestroyBuffer(m_dev, b.buf, nullptr);
		if (b.mem) vkFreeMemory(m_dev, b.mem, nullptr);
		b = Buf{};
	}

	bool make_image(Img &im, int w, int h, VkFormat fmt, VkImageUsageFlags usage, std::string &err)
	{
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {uint32_t(w), uint32_t(h), 1};
		ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKCHECK(vkCreateImage(m_dev, &ii, nullptr, &im.img), "vkCreateImage");
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(m_dev, im.img, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKCHECK(vkAllocateMemory(m_dev, &ai, nullptr, &im.mem), "vkAllocateMemory(image)");
		vkBindImageMemory(m_dev, im.img, im.mem, 0);
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = im.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VKCHECK(vkCreateImageView(m_dev, &vi, nullptr, &im.view), "vkCreateImageView");
		return true;
	}

	void destroy_img(Img &im)
	{
		if (im.view) vkDestroyImageView(m_dev, im.view, nullptr);
		if (im.img) vkDestroyImage(m_dev, im.img, nullptr);
		if (im.mem) vkFreeMemory(m_dev, im.mem, nullptr);
		im = Img{};
	}

	void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
	             VkAccessFlags sa, VkAccessFlags da)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(m_cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	// ---- command buffer management ---------------------------------------------------------------
	void begin_cb()
	{
		if (m_recording) return;
		vkResetCommandBuffer(m_cb, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(m_cb, &bi);
		m_recording = true;
	}

	void submit_wait()
	{
		if (!m_recording) return;
		vkEndCommandBuffer(m_cb);
		VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		sub.commandBufferCount = 1; sub.pCommandBuffers = &m_cb;
		vkQueueSubmit(m_q, 1, &sub, m_fence);
		vkWaitForFences(m_dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_dev, 1, &m_fence);
		m_recording = false;
		m_instbuf_off = m_stage_off = m_ubo_off = 0;
	}

	uint32_t write_params(const Params &p)
	{
		size_t align = std::max<size_t>(m_limits.minUniformBufferOffsetAlignment, 64);
		if (m_ubo_off + align > m_ubo.size) { submit_wait(); begin_cb(); }
		std::memcpy(m_ubo.map + m_ubo_off, &p, sizeof(p));
		uint32_t off = uint32_t(m_ubo_off);
		m_ubo_off += align;
		return off;
	}

	void copy_rows_to_image(Img &im, const void *src, size_t row_bytes, int width, int y0, int rows)
	{
		size_t bytes = row_bytes * size_t(rows);
		begin_cb();
		if (m_stage_off + bytes > m_stage.size) { submit_wait(); begin_cb(); }
		std::memcpy(m_stage.map + m_stage_off, src, bytes);
		barrier(im.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy c{};
		c.bufferOffset = m_stage_off;
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageOffset = {0, y0, 0};
		c.imageExtent = {uint32_t(width), uint32_t(rows), 1};
		vkCmdCopyBufferToImage(m_cb, m_stage.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		barrier(im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		m_stage_off += (bytes + 255) & ~size_t(255);
	}

	void begin_page_pass(int page)
	{
		int sz = 512 * m_opt.scale;
		VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = m_rp_page;
		rb.framebuffer = m_page_fb[page & 1];
		rb.renderArea = {{0, 0}, {uint32_t(sz), uint32_t(sz)}};
		vkCmdBeginRenderPass(m_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport vp{0, 0, float(sz), float(sz), 0, 1};
		VkRect2D sc{{0, 0}, {uint32_t(sz), uint32_t(sz)}};
		vkCmdSetViewport(m_cb, 0, 1, &vp);
		vkCmdSetScissor(m_cb, 0, 1, &sc);
	}

	void draw_rect_pass(int page, VkPipeline pipe, VkDescriptorSet set, const Params &p)
	{
		uint32_t dyn = write_params(p);
		begin_page_pass(page);
		vkCmdBindPipeline(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pl, 0, 1, &set, 1, &dyn);
		vkCmdDraw(m_cb, 4, 1, 0, 0);
		vkCmdEndRenderPass(m_cb);
	}

	// ---- resource creation -------------------------------------------------------------------------
	bool create_buffers(std::string &err)
	{
		return make_buffer(m_instbuf, INST_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, err) &&
		       make_buffer(m_stage, STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, err) &&
		       make_buffer(m_ubo, UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, err);
	}

	bool create_static_images(std::string &err)
	{
		const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (!make_image(m_tex_ram, 256, 16384, VK_FORMAT_R8_UINT, u, err)) return false;
		if (!make_image(m_tex_pal, 256, 128, VK_FORMAT_B8G8R8A8_UNORM, u, err)) return false;
		if (!make_image(m_tex_ovl, 512, 512, VK_FORMAT_R16_UINT, u, err)) return false;
		begin_cb();
		for (Img *im : {&m_tex_ram, &m_tex_pal, &m_tex_ovl})
			barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT);
		submit_wait();
		return true;
	}

	bool create_samplers(std::string &err)
	{
		VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		VKCHECK(vkCreateSampler(m_dev, &si, nullptr, &m_samp_nearest), "sampler");
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		VKCHECK(vkCreateSampler(m_dev, &si, nullptr, &m_samp_linear), "sampler");
		return true;
	}

	bool create_render_passes(std::string &err)
	{
		// page pass: load existing content, stay in SHADER_READ_ONLY between uses
		VkAttachmentDescription a{};
		a.format = VK_FORMAT_R8G8B8A8_UNORM; a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sp{};
		sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
		VkSubpassDependency deps[2]{};
		deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
		deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rp.attachmentCount = 1; rp.pAttachments = &a; rp.subpassCount = 1; rp.pSubpasses = &sp;
		rp.dependencyCount = 2; rp.pDependencies = deps;
		VKCHECK(vkCreateRenderPass(m_dev, &rp, nullptr, &m_rp_page), "render pass (page)");

		// swapchain pass: clear, present
		uint32_t fn = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(m_pd, m_surf, &fn, nullptr);
		std::vector<VkSurfaceFormatKHR> fmts(fn);
		vkGetPhysicalDeviceSurfaceFormatsKHR(m_pd, m_surf, &fn, fmts.data());
		m_sc_fmt = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
		for (auto &f : fmts)
			if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) m_sc_fmt = f;
		VkAttachmentDescription sa{};
		sa.format = m_sc_fmt.format; sa.samples = VK_SAMPLE_COUNT_1_BIT;
		sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		sa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; sa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		sa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; sa.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkSubpassDependency sd{};
		sd.srcSubpass = VK_SUBPASS_EXTERNAL; sd.dstSubpass = 0;
		sd.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; sd.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		sd.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		VkRenderPassCreateInfo sp2 = rp;
		sp2.pAttachments = &sa; sp2.dependencyCount = 1; sp2.pDependencies = &sd;
		VKCHECK(vkCreateRenderPass(m_dev, &sp2, nullptr, &m_rp_swap), "render pass (swap)");
		return true;
	}

	bool create_descriptors(std::string &err)
	{
		VkDescriptorSetLayoutBinding b[3]{};
		b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		b[2] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 3; li.pBindings = b;
		VKCHECK(vkCreateDescriptorSetLayout(m_dev, &li, nullptr, &m_dsl), "descriptor set layout");
		VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 1; pli.pSetLayouts = &m_dsl;
		VKCHECK(vkCreatePipelineLayout(m_dev, &pli, nullptr, &m_pl), "pipeline layout");

		VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}};
		VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.maxSets = 4; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
		VKCHECK(vkCreateDescriptorPool(m_dev, &pi, nullptr, &m_dpool), "descriptor pool");
		VkDescriptorSetLayout layouts[4] = {m_dsl, m_dsl, m_dsl, m_dsl};
		VkDescriptorSet sets[4];
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = m_dpool; ai.descriptorSetCount = 4; ai.pSetLayouts = layouts;
		VKCHECK(vkAllocateDescriptorSets(m_dev, &ai, sets), "vkAllocateDescriptorSets");
		m_set_quad = sets[0]; m_set_ovl = sets[1]; m_set_present[0] = sets[2]; m_set_present[1] = sets[3];

		write_set(m_set_quad, m_tex_ram.view, m_samp_nearest, m_tex_pal.view, m_samp_nearest);
		write_set(m_set_ovl, m_tex_ovl.view, m_samp_nearest, m_tex_pal.view, m_samp_nearest);
		return true;
	}

	void write_set(VkDescriptorSet set, VkImageView v0, VkSampler s0, VkImageView v1, VkSampler s1)
	{
		VkDescriptorImageInfo i0{s0, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo i1{s1, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo bi{m_ubo.buf, 0, sizeof(Params)};
		VkWriteDescriptorSet w[3]{};
		for (int i = 0; i < 3; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].descriptorCount = 1; }
		w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &i0;
		w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &i1;
		w[2].dstBinding = 3; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[2].pBufferInfo = &bi;
		vkUpdateDescriptorSets(m_dev, 3, w, 0, nullptr);
	}

	void write_present_sets()
	{
		VkSampler s = m_opt.smooth_output ? m_samp_linear : m_samp_nearest;
		for (int i = 0; i < 2; i++)
			if (m_page[i].view) write_set(m_set_present[i], m_page[i].view, s, m_tex_pal.view, m_samp_nearest);
	}

	VkShaderModule make_module(const uint32_t *code, size_t bytes)
	{
		VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = bytes; mi.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(m_dev, &mi, nullptr, &m);
		return m;
	}

	bool make_pipeline(VkPipeline &out, VkRenderPass rp, VkShaderModule vs, VkShaderModule fs, bool instanced, std::string &err)
	{
		VkPipelineShaderStageCreateInfo st[2]{};
		st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
		st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

		VkVertexInputBindingDescription bind{0, sizeof(GpuQuad), VK_VERTEX_INPUT_RATE_INSTANCE};
		VkVertexInputAttributeDescription at[5]{};
		for (uint32_t i = 0; i < 4; i++) at[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
		at[4] = {4, 0, VK_FORMAT_R32G32B32A32_UINT, 64};
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		if (instanced) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind; vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = at; }

		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1; vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1; cb.pAttachments = &cba;
		VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
		gp.pDynamicState = &ds; gp.layout = m_pl; gp.renderPass = rp;
		VKCHECK(vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gp, nullptr, &out), "vkCreateGraphicsPipelines");
		return true;
	}

	bool create_pipelines(std::string &err)
	{
		VkShaderModule qv = make_module(spv_quad_vert, sizeof(spv_quad_vert));
		VkShaderModule qf = make_module(spv_quad_frag, sizeof(spv_quad_frag));
		VkShaderModule rv = make_module(spv_rect_vert, sizeof(spv_rect_vert));
		VkShaderModule pf = make_module(spv_present_frag, sizeof(spv_present_frag));
		VkShaderModule of = make_module(spv_overlay_frag, sizeof(spv_overlay_frag));
		bool ok = make_pipeline(m_pipe_quad, m_rp_page, qv, qf, true, err) &&
		          make_pipeline(m_pipe_ovl, m_rp_page, rv, of, false, err) &&
		          make_pipeline(m_pipe_present, m_rp_swap, rv, pf, false, err);
		for (VkShaderModule m : {qv, qf, rv, pf, of}) vkDestroyShaderModule(m_dev, m, nullptr);
		return ok;
	}

	bool create_pages(std::string &err)
	{
		int sz = 512 * m_opt.scale;
		for (int i = 0; i < 2; i++)
		{
			if (!make_image(m_page[i], sz, sz, VK_FORMAT_R8G8B8A8_UNORM,
			                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, err))
				return false;
			VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			fi.renderPass = m_rp_page; fi.attachmentCount = 1; fi.pAttachments = &m_page[i].view;
			fi.width = fi.height = uint32_t(sz); fi.layers = 1;
			VKCHECK(vkCreateFramebuffer(m_dev, &fi, nullptr, &m_page_fb[i]), "framebuffer");
		}
		begin_cb();
		for (int i = 0; i < 2; i++)
		{
			barrier(m_page[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
			VkClearColorValue cc{{0, 0, 0, 1}};
			VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdClearColorImage(m_cb, m_page[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
			barrier(m_page[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		}
		submit_wait();
		write_present_sets();
		return true;
	}

	void destroy_pages()
	{
		for (int i = 0; i < 2; i++)
		{
			if (m_page_fb[i]) { vkDestroyFramebuffer(m_dev, m_page_fb[i], nullptr); m_page_fb[i] = VK_NULL_HANDLE; }
			destroy_img(m_page[i]);
		}
	}

	bool create_swapchain(std::string &err)
	{
		vkDeviceWaitIdle(m_dev);
		destroy_swapchain();
		VkSurfaceCapabilitiesKHR caps;
		VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_pd, m_surf, &caps), "surface caps");
		VkExtent2D ext = caps.currentExtent;
		if (ext.width == 0xffffffffu) ext = {uint32_t(m_win_w), uint32_t(m_win_h)};
		if (ext.width == 0 || ext.height == 0) { m_sc_dirty = true; return true; }

		uint32_t pn = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(m_pd, m_surf, &pn, nullptr);
		std::vector<VkPresentModeKHR> modes(pn);
		vkGetPhysicalDeviceSurfacePresentModesKHR(m_pd, m_surf, &pn, modes.data());
		VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
		if (!m_opt.vsync)
		{
			for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
			if (mode == VK_PRESENT_MODE_FIFO_KHR)
				for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
		}

		uint32_t count = std::max(caps.minImageCount + 1, 3u);
		if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
		VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = m_surf; sci.minImageCount = count; sci.imageFormat = m_sc_fmt.format; sci.imageColorSpace = m_sc_fmt.colorSpace;
		sci.imageExtent = ext; sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sci.preTransform = caps.currentTransform;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sci.presentMode = mode; sci.clipped = VK_TRUE;
		VKCHECK(vkCreateSwapchainKHR(m_dev, &sci, nullptr, &m_sc), "vkCreateSwapchainKHR");
		m_sc_ext = ext;

		uint32_t n = 0;
		vkGetSwapchainImagesKHR(m_dev, m_sc, &n, nullptr);
		m_sc_img.resize(n);
		vkGetSwapchainImagesKHR(m_dev, m_sc, &n, m_sc_img.data());
		m_sc_view.resize(n); m_sc_fb.resize(n);
		for (uint32_t i = 0; i < n; i++)
		{
			VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vi.image = m_sc_img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = m_sc_fmt.format;
			vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			VKCHECK(vkCreateImageView(m_dev, &vi, nullptr, &m_sc_view[i]), "swapchain view");
			VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			fi.renderPass = m_rp_swap; fi.attachmentCount = 1; fi.pAttachments = &m_sc_view[i];
			fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
			VKCHECK(vkCreateFramebuffer(m_dev, &fi, nullptr, &m_sc_fb[i]), "swapchain framebuffer");
		}
		m_sc_dirty = false;
		return true;
	}

	void destroy_swapchain()
	{
		for (auto f : m_sc_fb) vkDestroyFramebuffer(m_dev, f, nullptr);
		for (auto v : m_sc_view) vkDestroyImageView(m_dev, v, nullptr);
		m_sc_fb.clear(); m_sc_view.clear(); m_sc_img.clear();
		if (m_sc) { vkDestroySwapchainKHR(m_dev, m_sc, nullptr); m_sc = VK_NULL_HANDLE; }
	}

	HWND m_hwnd = nullptr;
	VideoOptions m_opt;
	int m_win_w = 1, m_win_h = 1;

	VkInstance m_inst = VK_NULL_HANDLE;
	VkSurfaceKHR m_surf = VK_NULL_HANDLE;
	VkPhysicalDevice m_pd = VK_NULL_HANDLE;
	VkPhysicalDeviceLimits m_limits{};
	VkPhysicalDeviceMemoryProperties m_memprops{};
	VkDevice m_dev = VK_NULL_HANDLE;
	VkQueue m_q = VK_NULL_HANDLE;
	uint32_t m_qfam = 0;
	int m_score = 0;
	char m_devname[256] = {};

	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_cb = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;
	VkSemaphore m_sem_acq = VK_NULL_HANDLE, m_sem_rel = VK_NULL_HANDLE;
	bool m_recording = false;

	Buf m_instbuf, m_stage, m_ubo;
	size_t m_instbuf_off = 0, m_stage_off = 0, m_ubo_off = 0;
	Img m_tex_ram, m_tex_pal, m_tex_ovl;
	Img m_page[2];
	VkFramebuffer m_page_fb[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkSampler m_samp_nearest = VK_NULL_HANDLE, m_samp_linear = VK_NULL_HANDLE;

	VkRenderPass m_rp_page = VK_NULL_HANDLE, m_rp_swap = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_dsl = VK_NULL_HANDLE;
	VkPipelineLayout m_pl = VK_NULL_HANDLE;
	VkDescriptorPool m_dpool = VK_NULL_HANDLE;
	VkDescriptorSet m_set_quad = VK_NULL_HANDLE, m_set_ovl = VK_NULL_HANDLE, m_set_present[2] = {};
	VkPipeline m_pipe_quad = VK_NULL_HANDLE, m_pipe_present = VK_NULL_HANDLE, m_pipe_ovl = VK_NULL_HANDLE;

	VkSwapchainKHR m_sc = VK_NULL_HANDLE;
	VkSurfaceFormatKHR m_sc_fmt{};
	VkExtent2D m_sc_ext{};
	std::vector<VkImage> m_sc_img;
	std::vector<VkImageView> m_sc_view;
	std::vector<VkFramebuffer> m_sc_fb;
	bool m_sc_dirty = false;
};

} // namespace

std::unique_ptr<IVideoBackend> create_vk_backend() { return std::make_unique<VkBackend>(); }
