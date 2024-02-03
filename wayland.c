#include <wayland-client.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <linux/input.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1.h"

#define MAGIC 	0x225def24

#define TRUE 	1
#define FALSE	0

#define UNUSED(x) ((void) x)

struct wl_state {
	// This is to make sure we passed the correct pointer to a function
	int magic;
	int running;
	int width, height;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct wl_seat *seat;
	struct wl_pointer *pointer;

	struct xdg_surface *xdg_surface;
	struct xdg_wm_base *wm_base;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *decoration;
	struct zxdg_decoration_manager_v1 *decor_mgr;
};

static struct wl_state *state;

static void randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	UNUSED(data);
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct wl_buffer *draw_frame(void)
{

	int stride = state->width * 4;
	int size = stride * state->height;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		return NULL;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			state->width, state->height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	int color;

	/* Draw checkerboxed background */
	for (int y = 0; y < state->height; ++y) {
		color = 0xFFFFFFFF;
		for (int x = 0; x < state->width; ++x) {
			if(x >= (7*state->width/8))
				color = 0xFF000000;
			else if(x >= (6*state->width/8))
				color = 0xFF0000FF;
			else if(x >= (5*state->width/8))
				color = 0xFFFF0000;
			else if(x >= (4*state->width/8))
				color = 0xFFFF00FF;
			else if(x >= (3*state->width/8))
				color = 0xFF00FF00;
			else if(x >= (2*state->width/8))
				color = 0xFF00FFFF;
			else if(x >= (1*state->width/8))
				color = 0xFFFFFF00;
			data[y * state->width + x] = color;
		}
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void xdg_surface_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct wl_state *state = data;

	xdg_surface_ack_configure(xdg_surface, serial);

	struct wl_buffer *buffer = draw_frame();
	wl_surface_attach(state->surface, buffer, 0, 0);
	wl_surface_commit(state->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, 
			     uint32_t serial)
{
	UNUSED(data);
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel, 
			       int width, int height, struct wl_array *states) 
{
	UNUSED(states);
	UNUSED(toplevel);

	struct wl_state *s = data;

	if(width == 0 || height == 0)
		return;

	s->width = width;
	s->height = height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) 
{
	UNUSED(data);
	UNUSED(toplevel);
	state->running = FALSE;
}

static const struct xdg_toplevel_listener toplevel_listener = {

	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
	UNUSED(version);

	struct wl_state *s = data;

	assert(s->magic == MAGIC);

	if (!strcmp(interface, wl_shm_interface.name)) {
		s->shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 
				          1);
	} else if (!strcmp(interface, wl_compositor_interface.name)) {
		s->compositor = wl_registry_bind(wl_registry, name, 
				                 &wl_compositor_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		s->seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		s->wm_base = wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(state->wm_base,
				&xdg_wm_base_listener, state);
	} else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
		s->decor_mgr = wl_registry_bind(wl_registry, name, 
				&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void
registry_global_remove(void *data,
        struct wl_registry *registry, uint32_t name)
{
	UNUSED(data);
	UNUSED(registry);
	UNUSED(name);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};


static void pointer_enter(void *data, struct wl_pointer *pointer, 
			uint32_t serial, struct wl_surface *surface,
			wl_fixed_t sx, wl_fixed_t sy)
{
	UNUSED(data);
	UNUSED(pointer);
	UNUSED(serial);
	UNUSED(surface);
	UNUSED(sx);
	UNUSED(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, 
			uint32_t serial, struct wl_surface *surface)
{
	UNUSED(data);
	UNUSED(pointer);
	UNUSED(serial);
	UNUSED(surface);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, 
			uint32_t serial, wl_fixed_t sx, wl_fixed_t sy)
{
	UNUSED(data);
	UNUSED(pointer);
	UNUSED(serial);
	UNUSED(sx);
	UNUSED(sy);
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, uint32_t time, uint32_t button,
		      uint32_t state)
{
	UNUSED(wl_pointer);
	UNUSED(time);

	struct wl_state *s = data;

	if (!s->toplevel)
		return;

	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		xdg_toplevel_move(s->toplevel, s->seat, serial);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
};

int main(void)
{
	state = malloc(sizeof(*state));

	state->magic = MAGIC;
	state->running = TRUE;

	state->display = wl_display_connect(NULL);
	if(!state->display) {
		fprintf(stderr, "Failed to connect to display: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	/* initial state */
	state->width = 640;
	state->height = 480;

	state->registry = wl_display_get_registry(state->display);

	wl_registry_add_listener(state->registry, &registry_listener, 
				 state);
	wl_display_roundtrip(state->display);

	state->pointer = wl_seat_get_pointer(state->seat);
	wl_pointer_add_listener(state->pointer, &pointer_listener, state);

	state->surface = wl_compositor_create_surface(state->compositor);
	state->xdg_surface = xdg_wm_base_get_xdg_surface(state->wm_base, 
 							 state->surface);
	xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, 
				 state);
	state->toplevel = xdg_surface_get_toplevel(state->xdg_surface);
	xdg_toplevel_set_app_id(state->toplevel, "xyz.ydalton.WaylandTest");
	xdg_toplevel_set_title(state->toplevel, "Example client");
	xdg_toplevel_add_listener(state->toplevel, &toplevel_listener, state);
#if 0
	state->decoration = 
		zxdg_decoration_manager_v1_get_toplevel_decoration(state->decor_mgr, 
								   state->toplevel);
#endif
	wl_surface_commit(state->surface);

	while (wl_display_dispatch(state->display) && state->running) {
	}
	zxdg_decoration_manager_v1_destroy(state->decor_mgr);
	xdg_toplevel_destroy(state->toplevel);
	xdg_surface_destroy(state->xdg_surface);
	wl_surface_destroy(state->surface);
	wl_pointer_release(state->pointer);
	wl_display_disconnect(state->display);
	free(state);
	return EXIT_SUCCESS;
}
