/*
 * BlueALSA - asound/logging.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_ASOUND_LOGGING_H_
#define BLUEALSA_ASOUND_LOGGING_H_
#endif

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <alsa/version.h>

#if SND_LIB_VERSION >= 0x01020F

# include <alsa/error.h>

# if DEBUG
#  define debug(M, ... ) snd_lib_log(SND_LOG_DEBUG, SND_ILOG_PCM, \
			__FILE__, __LINE__, __func__, 0, M, ##__VA_ARGS__)
#  define debug2(M, ... ) snd_lib_log(SND_LOG_DEBUG, SND_ILOG_PCM, \
			__FILE__, __LINE__, __func__, 0, "%s: " M, pcm->ba_pcm.pcm_path, \
			##__VA_ARGS__)
#  define debug2_params(M, ... ) snd_lib_log(SND_LOG_DEBUG, \
			SND_ILOG_PCM_PARAMS, __FILE__, __LINE__, __func__, 0, "%s: " M, \
			pcm->ba_pcm.pcm_path, ##__VA_ARGS__)
# else
#  define debug(M, ...) do {} while (0)
#  define debug2(M, ...) do {} while (0)
#  define debug2_params(M, ...) do {} while (0)
# endif

# define info( ... ) snd_lib_log(SND_LOG_INFO, \
			SND_ILOG_PCM, __FILE__, __LINE__, __func__, 0, ##__VA_ARGS__)
# define warn( ... ) snd_lib_log(SND_LOG_WARN, \
			SND_ILOG_PCM, __FILE__, __LINE__, __func__, 0, ##__VA_ARGS__)
# define error( ... ) snd_lib_log(SND_LOG_ERROR, \
			SND_ILOG_PCM, __FILE__, __LINE__, __func__, 0, ##__VA_ARGS__)

# define logging_init() do {} while (0)

#else /* SND_LIB_VERSION < 0x01020F */

# include "shared/log.h"

# ifdef SNDERR
# undef SNDERR
# endif
# define SNDERR(M, ...) \
		error(M, ## __VA_ARGS__)

# define debug2(M, ...) \
		debug("%s: " M, pcm->ba_pcm.pcm_path, ## __VA_ARGS__)
# define debug2_params debug2

static inline void logging_init(void) {
	const char *env_log_level = getenv("BLUEALSA_LOG_LEVEL");
	if (env_log_level && *env_log_level) {
		if (strcmp(env_log_level, "error") == 0)
			log_level = LOG_ERR;
		else if (strcmp(env_log_level, "warning") == 0)
			log_level = LOG_WARNING;
		else if (strcmp(env_log_level, "info") == 0)
			log_level = LOG_INFO;
		else if (strcmp(env_log_level, "debug") == 0)
			log_level = LOG_DEBUG;
	}
}

#endif


