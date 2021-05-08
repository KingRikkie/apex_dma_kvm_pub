#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <random>
#include <chrono>
#include <iostream>
#include <cfloat>
#include "Game.h"
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

FILE* dfile;

bool active = true;
uintptr_t aimentity = 0;
uintptr_t tmp_aimentity = 0;
uintptr_t lastaimentity = 0;
float max = 999.0f;
float max_dist = 300.0f*40.0f;	// ESP & Glow distance in meters (*40)
int localTeamId = 0;
int tmp_spec = 0, spectators = 0;
int tmp_all_spec = 0, allied_spectators = 0;
float max_fov = 3.0f;
int toRead = 100;
int aim = 0; 					// 0 = off, 1 = on - no visibility check, 2 = on - use visibility check
int player_glow = 2;			// 0 = off, 1 = on - not visible through walls, 2 = on - visible through walls 
float recoil_control = 0.45f;	// recoil reduction by this value, 1 = 100% = no recoil
Vector last_sway = Vector();	// used to determine when to reduce recoil
bool item_glow = true;
bool firing_range = false;

bool actions_t = false;
bool aim_t = false;
bool vars_t = false;
bool item_t = false;
bool recoil_t = false;
uint64_t g_Base;
bool lock = false;

const char* printPipe = "/tmp/myfifo";	// output pipe
const char* pipeClearCmd = "\033[H\033[2J\033[3J";	// escaped 'clear' command
int shellOut = -1;

typedef struct player
{
	float dist = 0;
	int entity_team = 0;
	float boxMiddle = 0;
	float h_y = 0;
	float width = 0;
	float height = 0;
	float b_x = 0;
	float b_y = 0;
	bool knocked = false;
	bool visible = false;
	int health = 0;
	int shield = 0;
	char name[33] = { 0 };
}player;


struct Matrix
{
	float matrix[16];
};

float lastvis_aim[100];

//////////////////////////////////////////////////////////////////////////////////////////////////

void SetPlayerGlow(WinProcess& mem, Entity& LPlayer, Entity& Target, int index)
{
	if (player_glow >= 1)
	{
		if (LPlayer.getPosition().z < 8000.f && Target.getPosition().z < 8000.f)
		{
			if (!Target.isGlowing() || (int)Target.buffer[OFFSET_GLOW_THROUGH_WALLS_GLOW_VISIBLE_TYPE] != 1 || (int)Target.buffer[GLOW_FADE] != 872415232) {
				float currentEntityTime = 5000.f;
				if (!isnan(currentEntityTime) && currentEntityTime > 0.f) {
					GColor color;
					if (Target.getTeamId() == LPlayer.getTeamId())
					{
						color = { 0.f, 2.f, 3.f };
					}
					else if (!(firing_range) && (Target.isKnocked() || !Target.isAlive()))
					{
						color = { 3.f, 3.f, 3.f };
					}
					else if (Target.lastVisTime() > lastvis_aim[index] || (Target.lastVisTime() < 0.f && lastvis_aim[index] > 0.f))
					{
						color = { 0.f, 1.f, 0.f };
					}
					else
					{
						int shield = Target.getShield();

						if (shield > 100)
						{ //Heirloom armor - Red
							color = { 3.f, 0.f, 0.f };
						}
						else if (shield > 75)
						{ //Purple armor - Purple
							color = { 1.84f, 0.46f, 2.07f };
						}
						else if (shield > 50)
						{ //Blue armor - Light blue
							color = { 0.39f, 1.77f, 2.85f };
						}
						else if (shield > 0)
						{ //White armor - White
							color = { 2.f, 2.f, 2.f };
						}
						else if (Target.getHealth() > 50)
						{ //Above 50% HP - Orange
							color = { 3.5f, 1.8f, 0.f };
						}
						else
						{ //Below 50% HP - Light Red
							color = { 3.20f, 0.78f, 0.85f };
						}
					}
				
					Target.enableGlow(mem, color);
				}
			}
		}
		else if((player_glow == 0) && Target.isGlowing())
		{
			Target.disableGlow(mem);
		}
	}
}

void ProcessPlayer(WinProcess& mem, Entity& LPlayer, Entity& target, uint64_t entitylist, int index)
{
	int entity_team = target.getTeamId();
	bool obs = target.Observing(mem, entitylist);
	if (obs)
	{
		/*if(obs == LPlayer.ptr)
		{
			if (entity_team == localTeamId)
			{
				tmp_all_spec++;
			}
			else
			{
				tmp_spec++;
			}
		}*/
		tmp_spec++;
		return;
	}
	Vector EntityPosition = target.getPosition();
	Vector LocalPlayerPosition = LPlayer.getPosition();
	float dist = LocalPlayerPosition.DistTo(EntityPosition);
	if (dist > max_dist)
	{
		if (target.isGlowing())
		{
			target.disableGlow(mem);
		}
		return;
	}

	if (!target.isAlive()) return;

	if (!firing_range && (entity_team < 0 || entity_team>50)) return;
	
	if (entity_team == localTeamId) return;

	if(aim == 2)
	{
		if((target.lastVisTime() > lastvis_aim[index]))
		{
			float fov = CalculateFov(LPlayer, target);
			if (fov < max)
			{
				max = fov;
				tmp_aimentity = target.ptr;
			}
		}
		else
		{
			if(aimentity==target.ptr)
			{
				aimentity=tmp_aimentity=lastaimentity=0;
			}
		}
	}
	else
	{
		float fov = CalculateFov(LPlayer, target);
		if (fov < max)
		{
			max = fov;
			tmp_aimentity = target.ptr;
		}
	}

	SetPlayerGlow(mem, LPlayer, target, index);

	lastvis_aim[index] = target.lastVisTime();
}

void DoActions(WinProcess& mem)
{
	actions_t = true;
	while (actions_t)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		while (g_Base!=0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			uint64_t LocalPlayer = mem.Read<uint64_t>(g_Base + OFFSET_LOCAL_ENT);
			if (LocalPlayer == 0) continue;

			Entity LPlayer = getEntity(mem, LocalPlayer);

			localTeamId = LPlayer.getTeamId();
			if (localTeamId < 0 || localTeamId > 50)
			{
				continue;
			}
			uint64_t entitylist = g_Base + OFFSET_ENTITYLIST;

			uint64_t baseent = mem.Read<uint64_t>(entitylist);
			if (baseent == 0)
			{
				continue;
			}

			max = 999.0f;
			tmp_spec = 0;
			tmp_all_spec = 0;
			tmp_aimentity = 0;
			if (firing_range)
			{
				int c=0;
				for (int i = 0; i < 10000; i++)
				{
					uint64_t centity = mem.Read<uint64_t>(entitylist + ((uint64_t)i << 5));
					if (centity == 0) continue;
					if (LocalPlayer == centity) continue;

					Entity Target = getEntity(mem, centity);
					if (!Target.isDummy())
					{
						continue;
					}

					ProcessPlayer(mem, LPlayer, Target, entitylist, c);
					c++;
				}
			}
			else
			{
				for (int i = 0; i < toRead; i++)
				{
					uint64_t centity = mem.Read<uint64_t>(entitylist + ((uint64_t)i << 5));
					if (centity == 0) continue;
					if (LocalPlayer == centity) continue;

					Entity Target = getEntity(mem, centity);
					if (!Target.isPlayer())
					{
						continue;
					}
					
					int entity_team = Target.getTeamId();
					if (entity_team == localTeamId)
					{
						continue;
					}

					ProcessPlayer(mem, LPlayer, Target, entitylist, i);
				}
			}
			spectators = tmp_spec;
			allied_spectators = tmp_all_spec;
			if (!lock)
				aimentity = tmp_aimentity;
			else
				aimentity = lastaimentity;
		}
	}
	actions_t = false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////

player players[100];

static void item_glow_t(WinProcess& mem)
{
	item_t = true;
	while (item_t)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		int k = 0;
		while (g_Base!=0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			uint64_t entitylist = g_Base + OFFSET_ENTITYLIST;
			if (item_glow)
			{
				for (int i = 0; i < 10000; i++)
				{
					uint64_t centity = mem.Read<uint64_t>(entitylist + ((uint64_t)i << 5));
					if (centity == 0) continue;

					Item item = getItem(mem, centity);

					if(item.isItem() && !item.isGlowing())
					{
						item.enableGlow(mem);
					}
				}
				k = 1;
				std::this_thread::sleep_for(std::chrono::milliseconds(600));
			}
			else
			{		
				if (k==1)
				{
					for (int i = 0; i < 10000; i++)
					{
						uint64_t centity = mem.Read<uint64_t>(entitylist + ((uint64_t)i << 5));
						if (centity == 0) continue;

						Item item = getItem(mem, centity);

						if(item.isItem() && item.isGlowing())
						{
							item.disableGlow(mem);
						}
					}
					k = 0;
				}
			}	
		}
	}
	item_t = false;
}

static void RecoilLoop(WinProcess& mem) 
{
	recoil_t = true;
	while (recoil_t) 
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		while (g_Base != 0)  
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			uint64_t LocalPlayer = mem.Read<uint64_t>(g_Base + OFFSET_LOCAL_ENT);
			if (LocalPlayer == 0) continue;
			int attackState = mem.Read<int>(g_Base + OFFSET_IS_ATTACKING);
			if (attackState != 5) {
				if (last_sway.x != 0) {
					last_sway.x = 0;
					last_sway.y = 0;
				}
				continue; // is not firing
			}

			Entity LPlayer = getEntity(mem, LocalPlayer);
			Vector ViewAngles = LPlayer.GetViewAngles();
			Vector SwayAngles = LPlayer.GetSwayAngles();

			// calculate recoil angles
			Vector recoilAngles = SwayAngles - ViewAngles;
			Vector compensatedAngles = ViewAngles;
			if (recoilAngles.x == 0 || recoilAngles.y == 0 || (recoilAngles.x - last_sway.x) == 0 || (recoilAngles.y - last_sway.y) == 0) 
				continue;
				
			// reduce recoil angles by last recoil as sway is continous
			compensatedAngles.x -= ((recoilAngles.x - last_sway.x) * recoil_control);
			compensatedAngles.y -= ((recoilAngles.y - last_sway.y) * recoil_control);
			last_sway = recoilAngles;
			
			LPlayer.SetViewAngles(mem, compensatedAngles);
		}
	}
	recoil_t = false;
}

// Requires an open pipe
static void printToPipe(std::string msg, bool clearShell = false)
{
	char buf[80];
	if (clearShell) {
		strcpy(buf, pipeClearCmd);
		write(shellOut, buf, strlen(buf)+1);
	}
	strcpy(buf, msg.c_str());
	write(shellOut, buf, strlen(buf)+1);
}

static void DebugLoop(WinProcess& mem) 
{
	while (true) 
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		while (g_Base != 0)  
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			uint64_t LocalPlayer = mem.Read<uint64_t>(g_Base + OFFSET_LOCAL_ENT);
			if (LocalPlayer == 0) continue;

			Entity LPlayer = getEntity(mem, LocalPlayer);

			uint64_t wephandle = mem.Read<uint64_t>(LocalPlayer + OFFSET_WEAPON);
			wephandle &= 0xffff;
			uint64_t entitylist = g_Base + OFFSET_ENTITYLIST;
			uint64_t wep_entity = mem.Read<uint64_t>(entitylist + (wephandle << 5));
			int ammoInClip = mem.Read<int>(wep_entity + OFFSET_AMMO_IN_CLIP);

			int attackState = mem.Read<int>(g_Base + OFFSET_IS_ATTACKING);
			Vector LocalCamera = LPlayer.GetCamPos();
			Vector ViewAngles = LPlayer.GetViewAngles();
			Vector SwayAngles = LPlayer.GetSwayAngles();

			printToPipe("Attack State:\t" + std::to_string(attackState) + "\n", true);
			printToPipe("Local Camera:\t" + std::to_string(LocalCamera.x) + "." + std::to_string(LocalCamera.y) + "." + std::to_string(LocalCamera.z) + "\n");
			printToPipe("View Angles: \t" + std::to_string(ViewAngles.x) + "." + std::to_string(ViewAngles.y) + "." + std::to_string(ViewAngles.z) + "\n");
			printToPipe("Sway Angles: \t" + std::to_string(SwayAngles.x) + "." + std::to_string(SwayAngles.y) + "." + std::to_string(SwayAngles.z) + "\n");
			printToPipe("Ammo Count:  \t" + std::to_string(ammoInClip)  + "\n");
		}
	}
}

__attribute__((constructor))
static void init()
{
	FILE* out = stdout;
	const char* ap_proc = "r5apex.exe";

	pid_t pid;
	#if (LMODE() == MODE_EXTERNAL())
	FILE* pipe = popen("pidof qemu-system-x86_64", "r");
	fscanf(pipe, "%d", &pid);
	pclose(pipe);
	#else
	out = fopen("/tmp/testr.txt", "w");
	pid = getpid();
	#endif
	fprintf(out, "Using Mode: %s\n", TOSTRING(LMODE));

	dfile = out;

	try
	{
		printf("\nStarting apex context...\n");
		WinContext ctx_apex(pid);
		printf("\nStarting refresh process list context...\n");
		WinContext ctx_refresh(pid);
		printf("\n");
		bool apex_found = false;

		// start external terminal and open pipe to print to it
		// system("gnome-terminal -- cat /tmp/myfifo");
		// mkfifo(printPipe, 0666);
		// shellOut = open(printPipe, O_WRONLY);
		
		while (active)
		{
			if (!apex_found)
			{
				aim_t = false;
				actions_t = false;
				item_t = false;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				printf("Searching apex process...\n");
				ctx_apex.processList.Refresh();
				for (auto& i : ctx_apex.processList)
				{
					if (!strcasecmp(ap_proc, i.proc.name))
					{					
						PEB peb = i.GetPeb();
						short magic = i.Read<short>(peb.ImageBaseAddress);
						g_Base = peb.ImageBaseAddress;
						if (g_Base != 0)
						{
							apex_found = true;
							fprintf(out, "\nApex found %lx:\t%s\n", i.proc.pid, i.proc.name);
							fprintf(out, "\tBase:\t%lx\tMagic:\t%hx (valid: %hhx)\n", peb.ImageBaseAddress, magic, (char)(magic == IMAGE_DOS_SIGNATURE));
							std::thread actions(DoActions, std::ref(i));
							std::thread itemglow(item_glow_t, std::ref(i));
							std::thread recoil(RecoilLoop, std::ref(i));
							//std::thread debug(DebugLoop, std::ref(i));
							actions.detach();
							itemglow.detach();
							recoil.detach();
							//debug.detach();
						}
					}
				}
			}

			if (apex_found)
			{
				apex_found = false;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				ctx_refresh.processList.Refresh();
				for (auto& i : ctx_refresh.processList)
				{
					if (!strcasecmp(ap_proc, i.proc.name))
					{
						PEB peb = i.GetPeb();
						if(peb.ImageBaseAddress != 0)
						{
							if(actions_t)
								apex_found = true;
						}
					}
				}

				if(!apex_found)
				{
					g_Base = 0;
					active = false;
				}
				else
				{
					if(!apex_found)
					{
						g_Base = 0;
					}
				}
			}
		}
	} catch (VMException& e)
	{
		fprintf(out, "Initialization error: %d\n", e.value);
	}
	fclose(out);
	close(shellOut);
}

int main()
{
	return 0;
}
