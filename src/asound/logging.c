/*
 * BlueALSA - asound/logging.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "asound/logging.h"

#if SND_LIB_VERSION < 0x01020F

#include <pthread.h>


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int ba_snd_log_level = -1;

void logging_init(void) {
	pthread_mutex_lock(&mutex);
	if (ba_snd_log_level != -1)
		goto finish;

	const char *env_log_level = getenv("BLUEALSA_LOG_LEVEL");
	if (env_log_level && *env_log_level) {
		if (strcmp(env_log_level, "error") == 0)
			ba_snd_log_level = BA_LOG_ERR;
		else if (strcmp(env_log_level, "warning") == 0)
			ba_snd_log_level = BA_LOG_WARN;
		else if (strcmp(env_log_level, "info") == 0)
			ba_snd_log_level = BA_LOG_INFO;
		else
			ba_snd_log_level = BA_LOG_DEBUG;
	}
finish:
	pthread_mutex_unlock(&mutex);
}

#endif
