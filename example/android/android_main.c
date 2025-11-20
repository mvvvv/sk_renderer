#if defined(__ANDROID__)

#include <android_native_app_glue.h>
#include <pthread.h>
#include <stdbool.h>

// Forward declare main entrypoint from example/main.c
extern int main(int argc, char **argv);

// Android state tracking
static bool      android_initialized = false;
static bool      android_finish      = false;
static pthread_t sk_renderer_thread_id;

void* sk_renderer_thread(void* arg) {
	// Invoke the app's main function
	main(0, NULL);

	android_finish = true;
	return NULL;
}

static void android_on_cmd(struct android_app* state, int32_t cmd) {
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		if (state->window != NULL && !android_initialized) {
			android_initialized = true;

			// Kick off the sk_renderer thread
			pthread_create(&sk_renderer_thread_id, NULL, sk_renderer_thread, NULL);
		}
		break;
	case APP_CMD_DESTROY:
		// Request app shutdown
		android_finish = true;
		break;
	default:
		break;
	}
}

void android_main(struct android_app* state) {
	// Register our event callback
	state->onAppCmd     = android_on_cmd;
	android_initialized = false;
	android_finish      = false;

	// The main Android event loop
	while (state->destroyRequested == 0) {
		// Process system events
		int32_t              events;
		struct android_poll_source* source;
		if (ALooper_pollOnce(android_initialized ? 0 : -1, NULL, &events, (void**)&source) >= 0) {
			if (source) {
				source->process(state, source);
			}
		}

		if (android_finish) {
			android_finish = false;
			ANativeActivity_finish(state->activity);
		}
	}

	// Wait until sk_renderer thread has finished cleaning up
	pthread_join(sk_renderer_thread_id, NULL);
}

#endif
