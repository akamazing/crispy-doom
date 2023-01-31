//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Moving object handling. Spawn functions.
//

#include <stdio.h>

#include "i_system.hpp"
#include "z_zone.hpp"
#include "m_random.hpp"

#include "doomdef.hpp"
#include "p_local.hpp"
#include "sounds.hpp"

#include "st_stuff.hpp"
#include "hu_stuff.hpp"

#include "s_sound.hpp"
#include "s_musinfo.hpp" // [crispy] S_ParseMusInfo()
#include "i_swap.hpp" // [crispy] SHORT()
#include "w_wad.hpp" // [crispy] W_CacheLumpNum()

#include "doomstat.hpp"


void G_PlayerReborn (int player);
void P_SpawnMapThing (mapthing_t*	mthing);


//
// P_SetMobjState
// Returns true if the mobj is still present.
//
int test;

// Use a heuristic approach to detect infinite state cycles: Count the number
// of times the loop in P_SetMobjState() executes and exit with an error once
// an arbitrary very large limit is reached.

#define MOBJ_CYCLE_LIMIT 1000000

boolean
P_SetMobjState
( mobj_t*	mobj,
  statenum_t	state )
{
    state_t*	st;
    int	cycle_counter = 0;

    do
    {
	if (state == S_NULL)
	{
	    mobj->state = (state_t *) S_NULL;
	    P_RemoveMobj (mobj);
	    return false;
	}

	st = &states[state];
	mobj->state = st;
	mobj->tics = st->tics;
	mobj->sprite = st->sprite;
	mobj->frame = st->frame;

	// Modified handling.
	// Call action functions when the state is set
	if (st->action.acp3)
	    st->action.acp3(mobj, NULL, NULL); // [crispy] let pspr action pointers get called from mobj states
	
	state = st->nextstate;

	if (cycle_counter++ > MOBJ_CYCLE_LIMIT)
	{
	    I_Error("P_SetMobjState: Infinite state cycle detected!");
	}
    } while (!mobj->tics);
				
    return true;
}

// [crispy] return the latest "safe" state in a state sequence,
// so that no action pointer is ever called
static statenum_t P_LatestSafeState(statenum_t state)
{
    statenum_t safestate = S_NULL;
    static statenum_t laststate, lastsafestate;

    if (state == laststate)
    {
	return lastsafestate;
    }

    for (laststate = state; state != S_NULL; state = states[state].nextstate)
    {
	if (safestate == S_NULL)
	{
	    safestate = state;
	}

	if (states[state].action.acp1)
	{
	    safestate = S_NULL;
	}

	// [crispy] a state with -1 tics never changes
	if (states[state].tics == -1 || state == states[state].nextstate)
	{
	    break;
	}
    }

    return lastsafestate = safestate;
}

//
// P_ExplodeMissile  
//
static void P_ExplodeMissileSafe (mobj_t* mo, boolean safe)
{
    mo->momx = mo->momy = mo->momz = 0;

    P_SetMobjState (mo, safe ? P_LatestSafeState(mobjinfo[mo->type].deathstate) : mobjinfo[mo->type].deathstate);

    mo->tics -= safe ? Crispy_Random()&3 : P_Random()&3;

    if (mo->tics < 1)
	mo->tics = 1;

    mo->flags &= ~MF_MISSILE;
    // [crispy] missile explosions are translucent
    mo->flags |= MF_TRANSLUCENT;

    if (mo->info->deathsound)
	S_StartSound (mo, mo->info->deathsound);
}

void P_ExplodeMissile (mobj_t* mo)
{
    return P_ExplodeMissileSafe(mo, false);
}

//
// P_XYMovement  
//
#define STOPSPEED		0x1000
#define FRICTION		0xe800

void P_XYMovement (mobj_t* mo) 
{ 	
    fixed_t 	ptryx;
    fixed_t	ptryy;
    player_t*	player;
    fixed_t	xmove;
    fixed_t	ymove;
			
    if (!mo->momx && !mo->momy)
    {
	if (mo->flags & MF_SKULLFLY)
	{
	    // the skull slammed into something
	    mo->flags &= ~MF_SKULLFLY;
	    mo->momx = mo->momy = mo->momz = 0;

	    P_SetMobjState (mo, mo->info->spawnstate);
	}
	return;
    }
	
    player = mo->player;
		
    if (mo->momx > MAXMOVE)
	mo->momx = MAXMOVE;
    else if (mo->momx < -MAXMOVE)
	mo->momx = -MAXMOVE;

    if (mo->momy > MAXMOVE)
	mo->momy = MAXMOVE;
    else if (mo->momy < -MAXMOVE)
	mo->momy = -MAXMOVE;
		
    xmove = mo->momx;
    ymove = mo->momy;
	
    do
    {
	if (xmove > MAXMOVE/2 || ymove > MAXMOVE/2)
	{
	    ptryx = mo->x + xmove/2;
	    ptryy = mo->y + ymove/2;
	    xmove >>= 1;
	    ymove >>= 1;
	}
	else
	{
	    ptryx = mo->x + xmove;
	    ptryy = mo->y + ymove;
	    xmove = ymove = 0;
	}
		
	if (!P_TryMove (mo, ptryx, ptryy))
	{
	    // blocked move
	    if (mo->player)
	    {	// try to slide along it
		P_SlideMove (mo);
	    }
	    else if (mo->flags & MF_MISSILE)
	    {
		boolean safe = false;
		// explode a missile
		if (ceilingline &&
		    ceilingline->backsector &&
		    ceilingline->backsector->ceilingpic == skyflatnum)
		{
		    if (mo->z > ceilingline->backsector->ceilingheight)
		    {
		    // Hack to prevent missiles exploding
		    // against the sky.
		    // Does not handle sky floors.
		    P_RemoveMobj (mo);
		    return;
		    }
		    else
		    {
			safe = true;
		    }
		}
		P_ExplodeMissileSafe (mo, safe);
	    }
	    else
		mo->momx = mo->momy = 0;
	}
    } while (xmove || ymove);
    
    // slow down
    if (player && player->cheats & CF_NOMOMENTUM)
    {
	// debug option for no sliding at all
	mo->momx = mo->momy = 0;
	return;
    }

    if (mo->flags & (MF_MISSILE | MF_SKULLFLY) )
	return; 	// no friction for missiles ever
		
    if (mo->z > mo->floorz)
	return;		// no friction when airborne

    if (mo->flags & MF_CORPSE)
    {
	// do not stop sliding
	//  if halfway off a step with some momentum
	if (mo->momx > FRACUNIT/4
	    || mo->momx < -FRACUNIT/4
	    || mo->momy > FRACUNIT/4
	    || mo->momy < -FRACUNIT/4)
	{
	    if (mo->floorz != mo->subsector->sector->floorheight)
		return;
	}
    }

    if (mo->momx > -STOPSPEED
	&& mo->momx < STOPSPEED
	&& mo->momy > -STOPSPEED
	&& mo->momy < STOPSPEED
	&& (!player
	    || (player->cmd.forwardmove== 0
		&& player->cmd.sidemove == 0 ) ) )
    {
	// if in a walking frame, stop moving
	if ( player&&(unsigned)((player->mo->state - states)- S_PLAY_RUN1) < 4)
	    P_SetMobjState (player->mo, S_PLAY);
	
	mo->momx = 0;
	mo->momy = 0;
    }
    else
    {
	mo->momx = FixedMul (mo->momx, FRICTION);
	mo->momy = FixedMul (mo->momy, FRICTION);
    }
}

//
// P_ZMovement
//
void P_ZMovement (mobj_t* mo)
{
    fixed_t	dist;
    fixed_t	delta;
    
    // check for smooth step up
    if (mo->player && mo->z < mo->floorz)
    {
	mo->player->viewheight -= mo->floorz-mo->z;

	mo->player->deltaviewheight
	    = (VIEWHEIGHT - mo->player->viewheight)>>3;
    }
    
    // adjust height
    mo->z += mo->momz;
	
    if ( mo->flags & MF_FLOAT
	 && mo->target)
    {
	// float down towards target if too close
	if ( !(mo->flags & MF_SKULLFLY)
	     && !(mo->flags & MF_INFLOAT) )
	{
	    dist = P_AproxDistance (mo->x - mo->target->x,
				    mo->y - mo->target->y);
	    
	    delta =(mo->target->z + (mo->height>>1)) - mo->z;

	    if (delta<0 && dist < -(delta*3) )
		mo->z -= FLOATSPEED;
	    else if (delta>0 && dist < (delta*3) )
		mo->z += FLOATSPEED;			
	}
	
    }
    
    // clip movement
    if (mo->z <= mo->floorz)
    {
	// hit the floor

	// Note (id):
	//  somebody left this after the setting momz to 0,
	//  kinda useless there.
	//
	// cph - This was the a bug in the linuxdoom-1.10 source which
	//  caused it not to sync Doom 2 v1.9 demos. Someone
	//  added the above comment and moved up the following code. So
	//  demos would desync in close lost soul fights.
	// Note that this only applies to original Doom 1 or Doom2 demos - not
	//  Final Doom and Ultimate Doom.  So we test demo_compatibility *and*
	//  gamemission. (Note we assume that Doom1 is always Ult Doom, which
	//  seems to hold for most published demos.)
        //  
        //  fraggle - cph got the logic here slightly wrong.  There are three
        //  versions of Doom 1.9:
        //
        //  * The version used in registered doom 1.9 + doom2 - no bounce
        //  * The version used in ultimate doom - has bounce
        //  * The version used in final doom - has bounce
        //
        // So we need to check that this is either retail or commercial
        // (but not doom2)
	
	int correct_lost_soul_bounce = gameversion >= exe_ultimate;

	if (correct_lost_soul_bounce && mo->flags & MF_SKULLFLY)
	{
	    // the skull slammed into something
	    mo->momz = -mo->momz;
	}
	
	if (mo->momz < 0)
	{
	    // [crispy] delay next jump
	    if (mo->player)
		mo->player->jumpTics = 7;
	    if (mo->player
		&& mo->momz < -GRAVITY*8)	
	    {
		// Squat down.
		// Decrease viewheight for a moment
		// after hitting the ground (hard),
		// and utter appropriate sound.
		mo->player->deltaviewheight = mo->momz>>3;
		// [crispy] center view if not using permanent mouselook
		if (!crispy->mouselook)
		    mo->player->centering = true;
		// [crispy] dead men don't say "oof"
		if (mo->health > 0 || !crispy->soundfix)
		{
		// [NS] Landing sound for longer falls. (Hexen's calculation.)
		if (mo->momz < -GRAVITY * 12)
		{
		    S_StartSoundOptional(mo, sfx_plland, sfx_oof);
		}
		else
		S_StartSound (mo, sfx_oof);
		}
	    }
	    // [NS] Beta projectile bouncing.
	    if ( (mo->flags & MF_MISSILE) && (mo->flags & MF_BOUNCES) )
	    {
		mo->momz = -mo->momz;
	    }
	    else
	    {
	    mo->momz = 0;
	    }
	}
	mo->z = mo->floorz;


	// cph 2001/05/26 -
	// See lost soul bouncing comment above. We need this here for bug
	// compatibility with original Doom2 v1.9 - if a soul is charging and
	// hit by a raising floor this incorrectly reverses its Y momentum.
	//

        if (!correct_lost_soul_bounce && mo->flags & MF_SKULLFLY)
            mo->momz = -mo->momz;

	if ( (mo->flags & MF_MISSILE)
	     // [NS] Beta projectile bouncing.
	     && !(mo->flags & MF_NOCLIP) && !(mo->flags & MF_BOUNCES) )
	{
	    P_ExplodeMissile (mo);
	    return;
	}
    }
    else if (! (mo->flags & MF_NOGRAVITY) )
    {
	if (mo->momz == 0)
	    mo->momz = -GRAVITY*2;
	else
	    mo->momz -= GRAVITY;
    }
	
    if (mo->z + mo->height > mo->ceilingz)
    {
	// hit the ceiling
	if (mo->momz > 0)
	{
	// [NS] Beta projectile bouncing.
	    if ( (mo->flags & MF_MISSILE) && (mo->flags & MF_BOUNCES) )
	    {
		mo->momz = -mo->momz;
	    }
	    else
	    {
	    mo->momz = 0;
	    }
	}
	{
	    mo->z = mo->ceilingz - mo->height;
	}

	if (mo->flags & MF_SKULLFLY)
	{	// the skull slammed into something
	    mo->momz = -mo->momz;
	}
	
	if ( (mo->flags & MF_MISSILE)
	     && !(mo->flags & MF_NOCLIP) && !(mo->flags & MF_BOUNCES) )
	{
	    P_ExplodeMissile (mo);
	    return;
	}
    }
} 



//
// P_NightmareRespawn
//
void
P_NightmareRespawn (mobj_t* mobj)
{
    fixed_t		x;
    fixed_t		y;
    fixed_t		z; 
    subsector_t*	ss; 
    mobj_t*		mo;
    mapthing_t*		mthing;
		
    x = mobj->spawnpoint.x << FRACBITS; 
    y = mobj->spawnpoint.y << FRACBITS; 

    // somthing is occupying it's position?
    if (!P_CheckPosition (mobj, x, y) ) 
	return;	// no respwan

    // spawn a teleport fog at old spot
    // because of removal of the body?
    mo = P_SpawnMobj (mobj->x,
		      mobj->y,
		      mobj->subsector->sector->floorheight , MT_TFOG); 
    // initiate teleport sound
    S_StartSound (mo, sfx_telept);

    // spawn a teleport fog at the new spot
    ss = R_PointInSubsector (x,y); 

    mo = P_SpawnMobj (x, y, ss->sector->floorheight , MT_TFOG); 

    S_StartSound (mo, sfx_telept);

    // spawn the new monster
    mthing = &mobj->spawnpoint;
	
    // spawn it
    if (mobj->info->flags & MF_SPAWNCEILING)
	z = ONCEILINGZ;
    else
	z = ONFLOORZ;

    // inherit attributes from deceased one
    mo = P_SpawnMobj (x,y,z, mobj->type);
    mo->spawnpoint = mobj->spawnpoint;	
    mo->angle = ANG45 * (mthing->angle/45);

    // [crispy] count respawned monsters
    extrakills++;

    if (mthing->options & MTF_AMBUSH)
	mo->flags |= MF_AMBUSH;

    mo->reactiontime = 18;
	
    // remove the old monster,
    P_RemoveMobj (mobj);
}

// [crispy] support MUSINFO lump (dynamic music changing)
static inline void MusInfoThinker (mobj_t *thing)
{
	if (musinfo.mapthing != thing &&
	    thing->subsector->sector == players[displayplayer].mo->subsector->sector)
	{
		musinfo.lastmapthing = musinfo.mapthing;
		musinfo.mapthing = thing;
		musinfo.tics = leveltime ? 30 : 0;
	}
}

//
// P_MobjThinker
//
void P_MobjThinker (mobj_t* mobj)
{
    // [crispy] support MUSINFO lump (dynamic music changing)
    if (mobj->type == MT_MUSICSOURCE)
    {
	return MusInfoThinker(mobj);
    }
    // [crispy] suppress interpolation of player missiles for the first tic
    // and Archvile fire to mitigate it being spawned at the wrong location
    if (mobj->interp < 0)
    {
        mobj->interp++;
    }
    else
    // [AM] Handle interpolation unless we're an active player.
    if (!(mobj->player != NULL && mobj == mobj->player->mo))
    {
        // Assume we can interpolate at the beginning
        // of the tic.
        mobj->interp = true;

        // Store starting position for mobj interpolation.
        mobj->oldx = mobj->x;
        mobj->oldy = mobj->y;
        mobj->oldz = mobj->z;
        mobj->oldangle = mobj->angle;
    }

    // momentum movement
    if (mobj->momx
	|| mobj->momy
	|| (mobj->flags&MF_SKULLFLY) )
    {
	P_XYMovement (mobj);

	// FIXME: decent NOP/NULL/Nil function pointer please.
	if (mobj->thinker.function.acv == (actionf_v) (-1))
	    return;		// mobj was removed
    }
    if ( (mobj->z != mobj->floorz)
	 || mobj->momz )
    {
	P_ZMovement (mobj);
	
	// FIXME: decent NOP/NULL/Nil function pointer please.
	if (mobj->thinker.function.acv == (actionf_v) (-1))
	    return;		// mobj was removed
    }

    
    // cycle through states,
    // calling action functions at transitions
    if (mobj->tics != -1)
    {
	mobj->tics--;
		
	// you can cycle through multiple states in a tic
	if (!mobj->tics)
	    if (!P_SetMobjState (mobj, mobj->state->nextstate) )
		return;		// freed itself
    }
    else
    {
	// check for nightmare respawn
	if (! (mobj->flags & MF_COUNTKILL) )
	    return;

	if (!respawnmonsters)
	    return;

	mobj->movecount++;

	if (mobj->movecount < 12*TICRATE)
	    return;

	if ( leveltime&31 )
	    return;

	if (P_Random () > 4)
	    return;

	P_NightmareRespawn (mobj);
    }

}


//
// P_SpawnMobj
//
static mobj_t*
P_SpawnMobjSafe
( fixed_t	x,
  fixed_t	y,
  fixed_t	z,
  mobjtype_t	type,
  boolean safe )
{
    mobj_t*	mobj;
    state_t*	st;
    mobjinfo_t*	info;
	
    mobj = Z_Malloc<decltype(mobj)> (sizeof(*mobj), PU_LEVEL, NULL);
    memset (mobj, 0, sizeof (*mobj));
    info = &mobjinfo[type];
	
    mobj->type = type;
    mobj->info = info;
    mobj->x = x;
    mobj->y = y;
    mobj->radius = info->radius;
    mobj->height = info->height;
    mobj->flags = info->flags;
    mobj->health = info->spawnhealth;

    if (gameskill != sk_nightmare)
	mobj->reactiontime = info->reactiontime;
    
    mobj->lastlook = safe ? Crispy_Random () % MAXPLAYERS : P_Random () % MAXPLAYERS;
    // do not set the state with P_SetMobjState,
    // because action routines can not be called yet
    st = &states[safe ? P_LatestSafeState(info->spawnstate) : info->spawnstate];

    mobj->state = st;
    mobj->tics = st->tics;
    mobj->sprite = st->sprite;
    mobj->frame = st->frame;

    // set subsector and/or block links
    P_SetThingPosition (mobj);
	
    mobj->floorz = mobj->subsector->sector->floorheight;
    mobj->ceilingz = mobj->subsector->sector->ceilingheight;

    if (z == ONFLOORZ)
	mobj->z = mobj->floorz;
    else if (z == ONCEILINGZ)
	mobj->z = mobj->ceilingz - mobj->info->height;
    else 
	mobj->z = z;

    // [crispy] randomly flip corpse, blood and death animation sprites
    if (mobj->flags & MF_FLIPPABLE && !(mobj->flags & MF_SHOOTABLE))
    {
	mobj->health = (mobj->health & (int)~1) - (Crispy_Random() & 1);
    }
    
    // [AM] Do not interpolate on spawn.
    mobj->interp = false;

    // [AM] Just in case interpolation is attempted...
    mobj->oldx = mobj->x;
    mobj->oldy = mobj->y;
    mobj->oldz = mobj->z;
    mobj->oldangle = mobj->angle;

    // [crispy] height of the spawnstate's first sprite in pixels
    if (!info->actualheight)
    {
	const spritedef_t *const sprdef = &sprites[mobj->sprite];

	if (!sprdef->numframes || !(mobj->flags & (MF_SOLID|MF_SHOOTABLE)))
	{
		info->actualheight = info->height;
	}
	else
	{
		spriteframe_t *sprframe;
		int lump;
		patch_t *patch;

		sprframe = &sprdef->spriteframes[mobj->frame & FF_FRAMEMASK];
		lump = sprframe->lump[0];
		patch = W_CacheLumpNum(lump + firstspritelump, PU_CACHE);

		// [crispy] round up to the next integer multiple of 8
		info->actualheight = ((SHORT(patch->height) + 7) >> 3) << (FRACBITS + 3);
	}
    }

    mobj->thinker.function.acp1 = (actionf_p1)P_MobjThinker;
	
    P_AddThinker (&mobj->thinker);

    return mobj;
}

mobj_t*
P_SpawnMobj
( fixed_t	x,
  fixed_t	y,
  fixed_t	z,
  mobjtype_t	type )
{
	return P_SpawnMobjSafe(x, y, z, type, false);
}

//
// P_RemoveMobj
//
mapthing_t	itemrespawnque[ITEMQUESIZE];
int		itemrespawntime[ITEMQUESIZE];
int		iquehead;
int		iquetail;


void P_RemoveMobj (mobj_t* mobj)
{
    if ((mobj->flags & MF_SPECIAL)
	&& !(mobj->flags & MF_DROPPED)
	&& (mobj->type != MT_INV)
	&& (mobj->type != MT_INS))
    {
	itemrespawnque[iquehead] = mobj->spawnpoint;
	itemrespawntime[iquehead] = leveltime;
	iquehead = (iquehead+1)&(ITEMQUESIZE-1);

	// lose one off the end?
	if (iquehead == iquetail)
	    iquetail = (iquetail+1)&(ITEMQUESIZE-1);
    }
	
    // unlink from sector and block lists
    P_UnsetThingPosition (mobj);
    
    // [crispy] removed map objects may finish their sounds
    if (crispy->soundfull)
    {
	S_UnlinkSound(mobj);
    }
    else
    {
    // stop any playing sound
    S_StopSound (mobj);
    }
    
    // free block
    P_RemoveThinker ((thinker_t*)mobj);
}




//
// P_RespawnSpecials
//
void P_RespawnSpecials (void)
{
    fixed_t		x;
    fixed_t		y;
    fixed_t		z;
    
    subsector_t*	ss; 
    mobj_t*		mo;
    mapthing_t*		mthing;
    
    int			i;

    // only respawn items in deathmatch
    // AX: deathmatch 3 is a Crispy-specific change
    if (deathmatch != 2 && deathmatch != 3)
	return;	// 

    // nothing left to respawn?
    if (iquehead == iquetail)
	return;		

    // wait at least 30 seconds
    if (leveltime - itemrespawntime[iquetail] < 30*TICRATE)
	return;			

    mthing = &itemrespawnque[iquetail];
	
    x = mthing->x << FRACBITS; 
    y = mthing->y << FRACBITS; 
	  
    // spawn a teleport fog at the new spot
    ss = R_PointInSubsector (x,y); 
    mo = P_SpawnMobj (x, y, ss->sector->floorheight , MT_IFOG); 
    S_StartSound (mo, sfx_itmbk);

    // find which type to spawn
    for (i=0 ; i< NUMMOBJTYPES ; i++)
    {
	if (mthing->type == mobjinfo[i].doomednum)
	    break;
    }

    if (i >= NUMMOBJTYPES)
    {
        I_Error("P_RespawnSpecials: Failed to find mobj type with doomednum "
                "%d when respawning thing. This would cause a buffer overrun "
                "in vanilla Doom", mthing->type);
    }

    // spawn it
    if (mobjinfo[i].flags & MF_SPAWNCEILING)
	z = ONCEILINGZ;
    else
	z = ONFLOORZ;

    mo = P_SpawnMobj (x,y,z, i);
    mo->spawnpoint = *mthing;	
    mo->angle = ANG45 * (mthing->angle/45);

    // pull it from the que
    iquetail = (iquetail+1)&(ITEMQUESIZE-1);
}



// [crispy] weapon sound sources
degenmobj_t muzzles[MAXPLAYERS];

mobj_t *Crispy_PlayerSO (int p)
{
	return crispy->soundfull ? (mobj_t *) &muzzles[p] : players[p].mo;
}

//
// P_SpawnPlayer
// Called when a player is spawned on the level.
// Most of the player structure stays unchanged
//  between levels.
//
void P_SpawnPlayer (mapthing_t* mthing)
{
    player_t*		p;
    fixed_t		x;
    fixed_t		y;
    fixed_t		z;

    mobj_t*		mobj;

    int			i;

    if (mthing->type == 0)
    {
        return;
    }

    // not playing?
    if (!playeringame[mthing->type-1])
	return;					
		
    p = &players[mthing->type-1];

    if (p->playerstate == PST_REBORN)
	G_PlayerReborn (mthing->type-1);

    x 		= mthing->x << FRACBITS;
    y 		= mthing->y << FRACBITS;
    z		= ONFLOORZ;
    mobj	= P_SpawnMobj (x,y,z, MT_PLAYER);

    // set color translations for player sprites
    if (mthing->type > 1)		
	mobj->flags |= (mthing->type-1)<<MF_TRANSSHIFT;
		
    mobj->angle	= ANG45 * (mthing->angle/45);
    mobj->player = p;
    mobj->health = p->health;

    p->mo = mobj;
    p->playerstate = PST_LIVE;	
    p->refire = 0;
    p->message = NULL;
    p->damagecount = 0;
    p->bonuscount = 0;
    p->extralight = 0;
    p->fixedcolormap = 0;
    p->viewheight = VIEWHEIGHT;

    // [crispy] weapon sound source
    p->so = Crispy_PlayerSO(mthing->type-1);

    pspr_interp = false; // interpolate weapon bobbing

    // setup gun psprite
    P_SetupPsprites (p);
    
    // give all cards in death match mode
    if (deathmatch)
	for (i=0 ; i<NUMCARDS ; i++)
	    p->cards[i] = true;
			
    if (mthing->type-1 == consoleplayer)
    {
	// wake up the status bar
	ST_Start ();
	// wake up the heads up text
	HU_Start ();		
    }
}


//
// P_SpawnMapThing
// The fields of the mapthing should
// already be in host byte order.
//
void P_SpawnMapThing (mapthing_t* mthing)
{
    int			i;
    int			bit;
    mobj_t*		mobj;
    fixed_t		x;
    fixed_t		y;
    fixed_t		z;
    int			musid = 0;
		
    // count deathmatch start positions
    if (mthing->type == 11)
    {
	if (deathmatch_p < &deathmatchstarts[10])
	{
	    memcpy (deathmatch_p, mthing, sizeof(*mthing));
	    deathmatch_p++;
	}
	return;
    }

    if (mthing->type <= 0)
    {
        // Thing type 0 is actually "player -1 start".  
        // For some reason, Vanilla Doom accepts/ignores this.

        return;
    }
	
    // check for players specially
    if (mthing->type <= 4)
    {
	// save spots for respawning in network games
	playerstarts[mthing->type-1] = *mthing;
	playerstartsingame[mthing->type-1] = true;
	if (!deathmatch)
	    P_SpawnPlayer (mthing);

	return;
    }

    // check for appropriate skill level
    if (!netgame && (mthing->options & 16) )
	return;
		
    if (gameskill == sk_baby)
	bit = 1;
    else if (gameskill == sk_nightmare)
	bit = 4;
    else
	bit = 1<<(gameskill-1);

    // [crispy] warn about mapthings without any skill tag set
    if (!(mthing->options & (MTF_EASY|MTF_NORMAL|MTF_HARD)))
    {
	fprintf(stderr, "P_SpawnMapThing: Mapthing type %i without any skill tag at (%i, %i)\n",
	       mthing->type, mthing->x, mthing->y);
    }

    if (!(mthing->options & bit) )
	return;
	
    // [crispy] support MUSINFO lump (dynamic music changing)
    if (mthing->type >= 14100 && mthing->type <= 14164)
    {
	musid = mthing->type - 14100;
	mthing->type = mobjinfo[MT_MUSICSOURCE].doomednum;
    }

    // find which type to spawn
    for (i=0 ; i< NUMMOBJTYPES ; i++)
	if (mthing->type == mobjinfo[i].doomednum)
	    break;
	
    if (i==NUMMOBJTYPES)
    {
	// [crispy] ignore unknown map things
	fprintf (stderr, "P_SpawnMapThing: Unknown type %i at (%i, %i)\n",
		 mthing->type,
		 mthing->x, mthing->y);
	return;
    }
		
    // don't spawn keycards and players in deathmatch
    if (deathmatch && mobjinfo[i].flags & MF_NOTDMATCH)
	return;
		
    // don't spawn any monsters if -nomonsters
    if (nomonsters
	&& ( i == MT_SKULL
	     || (mobjinfo[i].flags & MF_COUNTKILL)) )
    {
	return;
    }
    
    // spawn it
    x = mthing->x << FRACBITS;
    y = mthing->y << FRACBITS;

    if (mobjinfo[i].flags & MF_SPAWNCEILING)
	z = ONCEILINGZ;
    else
	z = ONFLOORZ;
    
    mobj = P_SpawnMobj (x,y,z, i);
    mobj->spawnpoint = *mthing;

    if (mobj->tics > 0)
	mobj->tics = 1 + (P_Random () % mobj->tics);
    if (mobj->flags & MF_COUNTKILL)
	totalkills++;
    if (mobj->flags & MF_COUNTITEM)
	totalitems++;
		
    mobj->angle = ANG45 * (mthing->angle/45);
    if (mthing->options & MTF_AMBUSH)
	mobj->flags |= MF_AMBUSH;

    // [crispy] support MUSINFO lump (dynamic music changing)
    if (i == MT_MUSICSOURCE)
    {
	mobj->health = 1000 + musid;
    }

    // [crispy] blinking key or skull in the status bar
    if (mobj->sprite == SPR_BSKU)
	st_keyorskull[it_bluecard] = 3;
    else
    if (mobj->sprite == SPR_RSKU)
	st_keyorskull[it_redcard] = 3;
    else
    if (mobj->sprite == SPR_YSKU)
	st_keyorskull[it_yellowcard] = 3;
}



//
// GAME SPAWN FUNCTIONS
//


//
// P_SpawnPuff
//

void
P_SpawnPuff
( fixed_t	x,
  fixed_t	y,
  fixed_t	z )
{
    return P_SpawnPuffSafe(x, y, z, false);
}

void
P_SpawnPuffSafe
( fixed_t	x,
  fixed_t	y,
  fixed_t	z,
  boolean	safe )
{
    mobj_t*	th;
	
    z += safe ? (Crispy_SubRandom() << 10) : (P_SubRandom() << 10);

    th = P_SpawnMobjSafe (x,y,z, MT_PUFF, safe);
    th->momz = FRACUNIT;
    th->tics -= safe ? Crispy_Random()&3 : P_Random()&3;

    if (th->tics < 1)
	th->tics = 1;
	
    // don't make punches spark on the wall
    if (attackrange == MELEERANGE)
	P_SetMobjState (th, safe ? P_LatestSafeState(S_PUFF3) : S_PUFF3);
}



//
// P_SpawnBlood
// 
void
P_SpawnBlood
( fixed_t	x,
  fixed_t	y,
  fixed_t	z,
  int		damage,
  mobj_t*	target ) // [crispy] pass thing type
{
    mobj_t*	th;
	
    z += (P_SubRandom() << 10);
    th = P_SpawnMobj (x,y,z, MT_BLOOD);
    th->momz = FRACUNIT*2;
    th->tics -= P_Random()&3;

    if (th->tics < 1)
	th->tics = 1;
		
    if (damage <= 12 && damage >= 9)
	P_SetMobjState (th,S_BLOOD2);
    else if (damage < 9)
	P_SetMobjState (th,S_BLOOD3);

    // [crispy] connect blood object with the monster that bleeds it
    th->target = target;
}



//
// P_CheckMissileSpawn
// Moves the missile forward a bit
//  and possibly explodes it right there.
//
void P_CheckMissileSpawn (mobj_t* th)
{
    th->tics -= P_Random()&3;
    if (th->tics < 1)
	th->tics = 1;
    
    // move a little forward so an angle can
    // be computed if it immediately explodes
    th->x += (th->momx>>1);
    th->y += (th->momy>>1);
    th->z += (th->momz>>1);

    if (!P_TryMove (th, th->x, th->y))
	P_ExplodeMissile (th);
}

// Certain functions assume that a mobj_t pointer is non-NULL,
// causing a crash in some situations where it is NULL.  Vanilla
// Doom did not crash because of the lack of proper memory 
// protection. This function substitutes NULL pointers for
// pointers to a dummy mobj, to avoid a crash.

mobj_t *P_SubstNullMobj(mobj_t *mobj)
{
    if (mobj == NULL)
    {
        static mobj_t dummy_mobj;

        dummy_mobj.x = 0;
        dummy_mobj.y = 0;
        dummy_mobj.z = 0;
        dummy_mobj.flags = 0;

        mobj = &dummy_mobj;
    }

    return mobj;
}

//
// P_SpawnMissile
//
mobj_t*
P_SpawnMissile
( mobj_t*	source,
  mobj_t*	dest,
  mobjtype_t	type )
{
    mobj_t*	th;
    angle_t	an;
    int		dist;

    th = P_SpawnMobj (source->x,
		      source->y,
		      source->z + 4*8*FRACUNIT, type);
    
    if (th->info->seesound)
	S_StartSound (th, th->info->seesound);

    th->target = source;	// where it came from
    an = R_PointToAngle2 (source->x, source->y, dest->x, dest->y);

    // fuzzy player
    if (dest->flags & MF_SHADOW)
	an += P_SubRandom() << 20;

    th->angle = an;
    an >>= ANGLETOFINESHIFT;
    th->momx = FixedMul (th->info->speed, finecosine[an]);
    th->momy = FixedMul (th->info->speed, finesine[an]);
	
    dist = P_AproxDistance (dest->x - source->x, dest->y - source->y);
    dist = dist / th->info->speed;

    if (dist < 1)
	dist = 1;

    th->momz = (dest->z - source->z) / dist;
    P_CheckMissileSpawn (th);
	
    return th;
}


//
// P_SpawnPlayerMissile
// Tries to aim at a nearby monster
//
void
P_SpawnPlayerMissile
( mobj_t*	source,
  mobjtype_t	type )
{
    mobj_t*	th;
    angle_t	an;
    
    fixed_t	x;
    fixed_t	y;
    fixed_t	z;
    fixed_t	slope;
    
    extern void A_Recoil (player_t* player);

    // see which target is to be aimed at
    an = source->angle;
    if (critical->freeaim == FREEAIM_DIRECT)
    {
	slope = PLAYER_SLOPE(source->player);
    }
    else
    {
    slope = P_AimLineAttack (source, an, 16*64*FRACUNIT);
    
    if (!linetarget)
    {
	an += 1<<26;
	slope = P_AimLineAttack (source, an, 16*64*FRACUNIT);

	if (!linetarget)
	{
	    an -= 2<<26;
	    slope = P_AimLineAttack (source, an, 16*64*FRACUNIT);
	}

	if (!linetarget)
	{
	    an = source->angle;
	    if (critical->freeaim == FREEAIM_BOTH)
               slope = PLAYER_SLOPE(source->player);
	    else
	    slope = 0;
	}
    }
    }
		
    x = source->x;
    y = source->y;
    z = source->z + 4*8*FRACUNIT;
	
    th = P_SpawnMobj (x,y,z, type);

    if (th->info->seesound)
	S_StartSound (th, th->info->seesound);

    th->target = source;
    th->angle = an;
    th->momx = FixedMul( th->info->speed,
			 finecosine[an>>ANGLETOFINESHIFT]);
    th->momy = FixedMul( th->info->speed,
			 finesine[an>>ANGLETOFINESHIFT]);
    th->momz = FixedMul( th->info->speed, slope);
    // [crispy] suppress interpolation of player missiles for the first tic
    th->interp = -1;

    P_CheckMissileSpawn (th);

    A_Recoil (source->player);
}
