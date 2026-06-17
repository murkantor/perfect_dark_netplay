#include <ultra64.h>
#include "constants.h"
#include "game/challenge.h"
#include "game/mplayer/mplayer.h"
#include "bss.h"
#include "data.h"
#include "types.h"

void challengesInit(void)
{
	struct mpconfigfull *mpconfig;
	// Was u8 buffer[0x1ca] (the N64 sizeof(struct mpconfigfull)). The port widened
	// mpsetup.options to u64, growing mpconfigfull past 0x1ca, so challengeLoadConfig
	// overflowed this stack buffer -- benign on 64-bit desktop, but it clobbered the
	// return address on 32-bit Xbox (crash on return from challengesInit). Size it to
	// the actual struct (== 0x1ca on N64, so byte-identical there).
	u8 buffer[sizeof(struct mpconfigfull)];
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_MpChallenges); i++) {
		g_MpChallenges[i].availability = 0;
		g_MpChallenges[i].completions[0] = 0;
		g_MpChallenges[i].completions[1] = 0;
		g_MpChallenges[i].completions[2] = 0;
		g_MpChallenges[i].completions[3] = 0;

		mpconfig = challengeLoad(i, buffer, sizeof(buffer));
		challengeForceUnlockConfigFeatures(&mpconfig->config, g_MpChallenges[i].unlockfeatures, 16, i);
	}

	for (i = 0; i < mpGetNumPresets(); i++) {
		mpconfig = challengeLoadConfig(g_MpPresets[i].confignum, buffer, sizeof(buffer));
		challengeForceUnlockConfigFeatures(&mpconfig->config, g_MpPresets[i].requirefeatures, 16, -1);
	}

	challengeDetermineUnlockedFeatures();
}
