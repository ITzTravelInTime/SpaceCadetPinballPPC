#include "pch.h"
#include "pb.h"


#include "control.h"
#include "fullscrn.h"
#include "high_score.h"
#include "pinball.h"
#include "proj.h"
#include "render.h"
#include "loader.h"
#include "midi.h"
#include "nudge.h"
#include "options.h"
#include "timer.h"
#include "winmain.h"
#include "Sound.h"
#include "TBall.h"
#include "TDemo.h"
#include "TEdgeSegment.h"
#include "TLightGroup.h"
#include "TPlunger.h"
#include "TTableLayer.h"
#include "GroupData.h"
#include "partman.h"
#include "score.h"
#include "TPinballTable.h"
#include "TTextBox.h"

TPinballTable* pb::MainTable = nullptr;
DatFile* pb::record_table = nullptr;
int pb::time_ticks = 0;
GameModes pb::game_mode = GameModes::GameOver;
float pb::time_now = 0, pb::time_next = 0, pb::ball_speed_limit, pb::time_ticks_remainder = 0;
bool pb::FullTiltMode = false, pb::FullTiltDemoMode = false, pb::cheat_mode = false, pb::demo_mode = false;
std::string pb::DatFileName;


int pb::init()
{
	float projMat[12], zMin = 0, zScaler = 0;

	if (DatFileName.empty())
		return 1;
	auto dataFilePath = pinball::make_path_name(DatFileName);
	record_table = partman::load_records(dataFilePath.c_str(), FullTiltMode);

	auto useBmpFont = 0;
	pinball::get_rc_int(158, &useBmpFont);
	if (useBmpFont)
		score::load_msg_font("pbmsg_ft");

	if (!record_table)
		return 1;

	auto plt = (ColorRgba*)record_table->field_labeled("background", FieldTypes::Palette);
	gdrv::display_palette(plt);

	auto backgroundBmp = record_table->GetBitmap(record_table->record_labeled("background"));
	auto cameraInfoId = record_table->record_labeled("camera_info") + fullscrn::GetResolution();
	auto cameraInfo = (float*)record_table->field(cameraInfoId, FieldTypes::FloatArray);

	/*Full tilt: table size depends on resolution*/
	auto resInfo = &fullscrn::resolution_array[fullscrn::GetResolution()];

	if (cameraInfo)
	{
		memcpy(&projMat, cameraInfo, sizeof(float) * 4 * 3);
		cameraInfo += 12;

		auto projCenterX = resInfo->TableWidth * 0.5f;
		auto projCenterY = resInfo->TableHeight * 0.5f;
		auto projD = cameraInfo[0];
		proj::init(projMat, projD, projCenterX, projCenterY);
		zMin = cameraInfo[1];
		zScaler = cameraInfo[2];
	}

	render::init(nullptr, zMin, zScaler, resInfo->TableWidth, resInfo->TableHeight);
	gdrv::copy_bitmap(
		render::vscreen,
		backgroundBmp->Width,
		backgroundBmp->Height,
		backgroundBmp->XPosition,
		backgroundBmp->YPosition,
		backgroundBmp,
		0,
		0);

	loader::loadfrom(record_table);

	mode_change(GameModes::InGame);

	time_ticks = 0;
	timer::init(150);
	score::init();

	MainTable = new TPinballTable();

	high_score::read();
	ball_speed_limit = MainTable->BallList.at(0)->Offset * 200.0f;
	return 0;
}

int pb::uninit()
{
	score::unload_msg_font();
	loader::unload();
	delete record_table;
	high_score::write();
	delete MainTable;
	MainTable = nullptr;
	timer::uninit();
	render::uninit();
	return 0;
}

void pb::SelectDatFile(const std::vector<const char*>& dataSearchPaths)
{
	DatFileName.clear();
	FullTiltDemoMode = FullTiltMode = false;

	std::string datFileNames[3]
	{
		"CADET.DAT",
		options::get_string("Pinball Data", pinball::get_rc_string(168, 0)),
		"DEMO.DAT",
	};

	// Default game data test order: CADET.DAT, PINBALL.DAT, DEMO.DAT
	if (options::Options.Prefer3DPBGameData)
		std::swap(datFileNames[0], datFileNames[1]);
	for (auto path : dataSearchPaths)
	{
		if (DatFileName.empty() && path)
		{
			pinball::BasePath = path;
			for (auto datFileName : datFileNames)
			{
				auto datFilePath = pinball::make_path_name(datFileName);
				auto datFile = fopenu(datFilePath.c_str(), "r");
				if (datFile)
				{
					fclose(datFile);
					DatFileName = datFileName;
					if (datFileName == "CADET.DAT")
						FullTiltMode = true;
					if (datFileName == "DEMO.DAT")
						FullTiltDemoMode = FullTiltMode = true;

					printf("Loading game from: %s\n", datFilePath.c_str());
					break;
				}
			}
		}
	}
}

void pb::reset_table()
{
	if (MainTable)
		MainTable->Message(1024, 0.0);
}


void pb::firsttime_setup()
{
	render::update();
}

void pb::mode_change(GameModes mode)
{
	switch (mode)
	{
	case GameModes::InGame:
		if (demo_mode)
		{
			winmain::LaunchBallEnabled = false;
			winmain::HighScoresEnabled = false;
			winmain::DemoActive = true;
			if (MainTable)
			{
				if (MainTable->Demo)
					MainTable->Demo->ActiveFlag = 1;
			}
		}
		else
		{
			winmain::LaunchBallEnabled = true;
			winmain::HighScoresEnabled = true;
			winmain::DemoActive = false;
			if (MainTable)
			{
				if (MainTable->Demo)
					MainTable->Demo->ActiveFlag = 0;
			}
		}
		break;
	case GameModes::GameOver:
		winmain::LaunchBallEnabled = false;
		if (!demo_mode)
		{
			winmain::HighScoresEnabled = true;
			winmain::DemoActive = false;
		}
		if (MainTable && MainTable->LightGroup)
			MainTable->LightGroup->Message(29, 1.4f);
		break;
	}
	game_mode = mode;
}

void pb::toggle_demo()
{
	if (demo_mode)
	{
		demo_mode = false;
		MainTable->Message(1024, 0.0);
		mode_change(GameModes::GameOver);
		pinball::MissTextBox->Clear();
		auto text = pinball::get_rc_string(24, 0);
		pinball::InfoTextBox->Display(text, -1.0);
	}
	else
	{
		replay_level(true);
	}
}

void pb::replay_level(bool demoMode)
{
	demo_mode = demoMode;
	mode_change(GameModes::InGame);
	if (options::Options.Music)
		midi::play_pb_theme();
	MainTable->Message(1014, static_cast<float>(options::Options.Players));
}

void pb::ballset(float dx, float dy)
{
	// dx and dy are normalized to window, ideally in [-1, 1]
	static constexpr float sensitivity = 7000;
	TBall* ball = MainTable->BallList.at(0);
	ball->Acceleration.X = dx * sensitivity;
	ball->Acceleration.Y = dy * sensitivity;
	ball->Speed = maths::normalize_2d(ball->Acceleration);
}

void pb::frame(float dtMilliSec)
{
	if (dtMilliSec > 100)
		dtMilliSec = 100;
	if (dtMilliSec <= 0)
		return;

	float dtSec = dtMilliSec * 0.001f;
	time_next = time_now + dtSec;
	timed_frame(time_now, dtSec, true);
	time_now = time_next;

	dtMilliSec += time_ticks_remainder;
	auto dtWhole = static_cast<int>(dtMilliSec);
	time_ticks_remainder = dtMilliSec - static_cast<float>(dtWhole);
	time_ticks += dtWhole;

	if (nudge::nudged_left || nudge::nudged_right || nudge::nudged_up)
	{
		nudge::nudge_count = dtSec * 4.0f + nudge::nudge_count;
	}
	else
	{
		auto nudgeDec = nudge::nudge_count - dtSec;
		if (nudgeDec <= 0.0f)
			nudgeDec = 0.0;
		nudge::nudge_count = nudgeDec;
	}
	timer::check();
	render::update();
	score::update(MainTable->CurScoreStruct);
	if (!MainTable->TiltLockFlag)
	{
		if (nudge::nudge_count > 0.5f)
		{
			pinball::InfoTextBox->Display(pinball::get_rc_string(25, 0), 2.0);
		}
		if (nudge::nudge_count > 1.0f)
			MainTable->tilt(time_now);
	}
}

void pb::timed_frame(float timeNow, float timeDelta, bool drawBalls)
{
	vector2 vec1{}, vec2{};

	for (auto ball : MainTable->BallList)
	{
		if (ball->ActiveFlag != 0)
		{
			auto collComp = ball->CollisionComp;
			if (collComp)
			{
				ball->TimeDelta = timeDelta;
				collComp->FieldEffect(ball, &vec1);
			}
			else
			{
				if (MainTable->ActiveFlag)
				{
					vec2.X = 0.0;
					vec2.Y = 0.0;
					TTableLayer::edge_manager->FieldEffects(ball, &vec2);
					vec2.X = vec2.X * timeDelta;
					vec2.Y = vec2.Y * timeDelta;
					ball->Acceleration.X = ball->Speed * ball->Acceleration.X;
					ball->Acceleration.Y = ball->Speed * ball->Acceleration.Y;
					maths::vector_add(ball->Acceleration, vec2);
					ball->Speed = maths::normalize_2d(ball->Acceleration);
					ball->InvAcceleration.X = ball->Acceleration.X == 0.0f ? 1.0e9f : 1.0f / ball->Acceleration.X;
					ball->InvAcceleration.Y = ball->Acceleration.Y == 0.0f ? 1.0e9f : 1.0f / ball->Acceleration.Y;
				}

				auto timeDelta2 = timeDelta;
				auto timeNow2 = timeNow;
				for (auto index = 10; timeDelta2 > 0.000001f && index; --index)
				{
					auto time = collide(timeNow2, timeDelta2, ball);
					timeDelta2 -= time;
					timeNow2 += time;
				}
			}
		}
	}

	if (drawBalls)
	{
		for (auto ball : MainTable->BallList)
		{
			if (ball->ActiveFlag)
				ball->Repaint();
		}
	}
}

void pb::window_size(int* width, int* height)
{
	*width = fullscrn::resolution_array[fullscrn::GetResolution()].TableWidth;
	*height = fullscrn::resolution_array[fullscrn::GetResolution()].TableHeight;
}

void pb::pause_continue()
{
	winmain::single_step ^= true;
	pinball::InfoTextBox->Clear();
	pinball::MissTextBox->Clear();
	if (winmain::single_step)
	{
		if (MainTable)
			MainTable->Message(1008, time_now);
		pinball::InfoTextBox->Display(pinball::get_rc_string(22, 0), -1.0);
		midi::music_stop();
		Sound::Deactivate();
	}
	else
	{
		if (MainTable)
			MainTable->Message(1009, 0.0);
		if (!demo_mode)
		{
			char* text;
			float textTime;
			if (game_mode == GameModes::GameOver)
			{
				textTime = -1.0;
				text = pinball::get_rc_string(24, 0);
			}
			else
			{
				textTime = 5.0;
				text = pinball::get_rc_string(23, 0);
			}
			pinball::InfoTextBox->Display(text, textTime);
		}
		if (options::Options.Music && !winmain::single_step)
			midi::play_pb_theme();
		Sound::Activate();
	}
}

void pb::loose_focus()
{
	if (MainTable)
		MainTable->Message(1010, time_now);
}

void pb::InputUp(GameInput input)
{
	if (game_mode != GameModes::InGame || winmain::single_step || demo_mode)
		return;

	if (AnyBindingMatchesInput(options::Options.Key.LeftFlipper, input))
	{
		MainTable->Message(1001, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.RightFlipper, input))
	{
		MainTable->Message(1003, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.Plunger, input))
	{
		MainTable->Message(1005, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.LeftTableBump, input))
	{
		nudge::un_nudge_right(0, nullptr);
	}
	if (AnyBindingMatchesInput(options::Options.Key.RightTableBump, input))
	{
		nudge::un_nudge_left(0, nullptr);
	}
	if (AnyBindingMatchesInput(options::Options.Key.BottomTableBump, input))
	{
		nudge::un_nudge_up(0, nullptr);
	}
}

void pb::InputDown(GameInput input)
{
	options::InputDown(input);
	if (game_mode != GameModes::InGame || winmain::single_step || demo_mode)
		return;

	if (input.Type == InputTypes::Keyboard)
		control::pbctrl_bdoor_controller(static_cast<char>(input.Value));

	if (AnyBindingMatchesInput(options::Options.Key.LeftFlipper, input))
	{
		MainTable->Message(1000, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.RightFlipper, input))
	{
		MainTable->Message(1002, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.Plunger, input))
	{
		MainTable->Message(1004, time_now);
	}
	if (AnyBindingMatchesInput(options::Options.Key.LeftTableBump, input))
	{
		if (!MainTable->TiltLockFlag)
			nudge::nudge_right();
	}
	if (AnyBindingMatchesInput(options::Options.Key.RightTableBump, input))
	{
		if (!MainTable->TiltLockFlag)
			nudge::nudge_left();
	}
	if (AnyBindingMatchesInput(options::Options.Key.BottomTableBump, input))
	{
		if (!MainTable->TiltLockFlag)
			nudge::nudge_up();
	}

	if (input.Type == InputTypes::GameController && input.Value == SDL_CONTROLLER_BUTTON_BACK)
	{
		winmain::new_game();
	}
	
	if (cheat_mode && input.Type == InputTypes::Keyboard)
	{
		switch (input.Value)
		{
		case 'b':
			TBall* ball;
			if (MainTable->BallList.empty())
			{
				ball = new TBall(MainTable);
			}
			else
			{
				for (auto index = 0u; ;)
				{
					ball = MainTable->BallList.at(index);
					if (!ball->ActiveFlag)
						break;
					++index;
					if (index >= MainTable->BallList.size())
					{
						ball = new TBall(MainTable);
						break;
					}
				}
			}
			ball->ActiveFlag = 1;
			ball->Position.X = 1.0;
			ball->Position.Z = ball->Offset;
			ball->Position.Y = 1.0;
			ball->Acceleration.Z = 0.0;
			ball->Acceleration.Y = 0.0;
			ball->Acceleration.X = 0.0;
			break;
		case 'h':
		{
			high_score_struct entry{ {0}, 1000000000 };
			strncpy(entry.Name, pinball::get_rc_string(26, 0), sizeof entry.Name - 1);
			high_score::show_and_set_high_score_dialog({ entry, 1 });
			break;
		}
		case 'r':
			control::cheat_bump_rank();
			break;
		case 's':
			MainTable->AddScore(static_cast<int>(RandFloat() * 1000000.0f));
			break;
		case SDLK_F12:
			MainTable->port_draw();
			break;
		}
	}
}

void pb::launch_ball()
{
	MainTable->Plunger->Message(1017, 0.0f);
}

void pb::end_game()
{
	int scores[4]{};
	int scoreIndex[4]{};

	mode_change(GameModes::GameOver);
	int playerCount = MainTable->PlayerCount;

	score_struct_super* scorePtr = MainTable->PlayerScores;
	for (auto index = 0; index < playerCount; ++index)
	{
		scores[index] = scorePtr->ScoreStruct->Score;
		scoreIndex[index] = index;
		++scorePtr;
	}

	for (auto i = 0; i < playerCount; ++i)
	{
		for (auto j = i + 1; j < playerCount; ++j)
		{
			if (scores[j] > scores[i])
			{
				int score = scores[j];
				scores[j] = scores[i];
				scores[i] = score;

				int index = scoreIndex[j];
				scoreIndex[j] = scoreIndex[i];
				scoreIndex[i] = index;
			}
		}
	}

	if (!demo_mode && !MainTable->CheatsUsed)
	{
		for (auto i = 0; i < playerCount; ++i)
		{
			int position = high_score::get_score_position(scores[i]);
			if (position >= 0)
			{
				high_score_struct entry{ {0}, scores[i] };
				strncpy(entry.Name, pinball::get_rc_string(scoreIndex[i] + 26, 0), sizeof entry.Name - 1);
				high_score::show_and_set_high_score_dialog({ entry, -1 });
			}
		}
	}
}

void pb::high_scores()
{
	high_score::show_high_score_dialog();
}

void pb::tilt_no_more()
{
	if (MainTable->TiltLockFlag)
		pinball::InfoTextBox->Clear();
	MainTable->TiltLockFlag = 0;
	nudge::nudge_count = -2.0;
}

bool pb::chk_highscore()
{
	if (demo_mode)
		return false;
	for (auto i = 0; i < MainTable->PlayerCount; ++i)
	{
		if (high_score::get_score_position(MainTable->PlayerScores[i].ScoreStruct->Score) >= 0)
			return true;
	}
	return false;
}

float pb::collide(float timeNow, float timeDelta, TBall* ball)
{
	ray_type ray{};
	vector2 positionMod{};

	if (ball->ActiveFlag && !ball->CollisionComp)
	{
		if (ball_speed_limit < ball->Speed)
			ball->Speed = ball_speed_limit;

		auto maxDistance = timeDelta * ball->Speed;
		ball->TimeDelta = timeDelta;
		ball->RayMaxDistance = maxDistance;
		ball->TimeNow = timeNow;

		ray.Origin = ball->Position;
		ray.Direction = ball->Acceleration;
		ray.MaxDistance = maxDistance;
		ray.FieldFlag = ball->FieldFlag;
		ray.TimeNow = timeNow;
		ray.TimeDelta = timeDelta;
		ray.MinDistance = 0.0020000001f;

		TEdgeSegment* edge = nullptr;
		auto distance = TTableLayer::edge_manager->FindCollisionDistance(&ray, ball, &edge);
		ball->EdgeCollisionCount = 0;
		if (distance >= 1000000000.0f)
		{
			maxDistance = timeDelta * ball->Speed;
			ball->RayMaxDistance = maxDistance;
			positionMod.X = maxDistance * ball->Acceleration.X;
			positionMod.Y = maxDistance * ball->Acceleration.Y;
			maths::vector_add(ball->Position, positionMod);
		}
		else
		{
			edge->EdgeCollision(ball, distance);
			if (ball->Speed > 0.000000001f)
				return fabs(distance / ball->Speed);
		}
	}
	return timeDelta;
}

void pb::PushCheat(const std::string& cheat)
{
	for (auto ch : cheat)
		control::pbctrl_bdoor_controller(ch);
}

bool pb::AnyBindingMatchesInput(GameInput (&options)[3], GameInput key)
{
	for (auto& option : options)
		if (key == option)
			return true;
	return false;
}
