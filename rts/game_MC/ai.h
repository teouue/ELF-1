/**
* Copyright (c) 2017-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#pragma once

#include "engine/cmd_util.h"
#include "engine/cmd_interface.h"
#include "engine/game_state.h"
#include "elf/circular_queue.h"
#include "elf/ai.h"
#include "game_action.h"
#include "python_options.h"
#define NUM_RES_SLOT 5

using Comm = Context::Comm;
using AIComm = AICommT<Comm>;
using Data = typename AIComm::Data;

using AIWithComm = elf::AIWithCommT<RTSState, RTSMCAction, AIComm>;
using AI = elf::AI_T<RTSState, RTSMCAction>;

class TrainedAI : public AIWithComm {
public:
    using Data = typename AIWithComm::Data;

    TrainedAI() : _respect_fow(true), _recent_states(1) { }
    TrainedAI(const AIOptions &opt)
        : AIWithComm(opt.name, opt.fs), _respect_fow(opt.fow), _recent_states(opt.num_frames_in_state) {
      for (auto &v : _recent_states.v()) v.clear();
    }

    bool GameEnd(Tick) override;

protected:
    const bool _respect_fow;

    // History to send.
    CircularQueue<std::vector<float>> _recent_states;

    void compute_state(std::vector<float> *state);

    // Feature extraction.
    void extract(Data *data) override;
    bool handle_response(const Data &data, RTSMCAction *a) override;
};

// Simple AI, rule-based AI for Mini-RTS
class SimpleAI : public AI {
public:
    SimpleAI(const AIOptions &opt) : AI(opt.name, opt.fs)  { }

    // SERIALIZER_DERIVED(SimpleAI, AIBase, _state);

private:
    bool on_act(Tick, RTSMCAction *action, const atomic_bool *) override;
};

// HitAndRun AI, rule-based AI for Mini-RTS
class HitAndRunAI : public AI {
public:
    HitAndRunAI(const AIOptions &opt) : AI(opt.name, opt.fs)  { }

    // SERIALIZER_DERIVED(HitAndRunAI, AIBase, _state);

private:
    bool on_act(Tick, RTSMCAction *action, const atomic_bool *) override;
};

// Lua AI, rule-based AI for Mini-RTS
class LuaAI : public AI {
public:
    LuaAI(const AIOptions &opt) : AI(opt.name, opt.fs)  { }

    // SERIALIZER_DERIVED(LuaAI, AIBase, _state);

private:
    bool on_act(Tick, RTSMCAction *action, const atomic_bool *) override;
};

class MixedAI : public AI {
public:
    MixedAI(const AIOptions &opt) : AI(opt.name, opt.fs) {
          if (opt.args != "") {
              // TODO: Get some library to do this.
              std::string backup_ai_name;
              for (const auto &kv : _parse(opt.args)) {
                  if (kv.first == "start") _latest_start = std::stoi(kv.second);
                  else if (kv.first == "decay") _latest_start_decay = std::stof(kv.second);
                  else if (kv.first == "backup") {
                      AIOptions opt2;
                      opt2.fs = opt.fs;
                      if (kv.second == "AI_SIMPLE" || kv.second == "ai_simple") {
                          // std::cout << "Initialize backup as ai_simple" << std::endl;
                          _backup_ai.reset(new SimpleAI(opt2));
                      }
                      else if (kv.second == "AI_HIT_AND_RUN" || kv.second == "ai_hit_and_run") {
                          // std::cout << "Initialize backup as ai_hit_and_run" << std::endl;
                          _backup_ai.reset(new HitAndRunAI(opt2));
                      }
                  } else {
                      std::cout << "Unrecognized (key, value) = (" << kv.first << "," << kv.second << ")" << std::endl;
                  }
              }
              // std::cout << "Latest start = " << _latest_start << " decay = " << _latest_start_decay << std::endl;
              if (_backup_ai.get() != nullptr) {
                  _backup_ai->SetId(id());
                  if (s_ptr() != nullptr) _backup_ai->SetState(s());
              }
          }
          _rng.seed(time(NULL));
    }

    void SetMainAI(AI *main_ai) {
        _main_ai.reset(main_ai);
        _main_ai->SetId(id());
        if (s_ptr() != nullptr) _main_ai->SetState(s());
    }

    bool GameEnd(Tick t) override {
        AI::GameEnd(t);

        // Always ended with main_ai.
        bool res = _main_ai->GameEnd(t);

        // Decay latest_start.
        _latest_start *= _latest_start_decay;

        // Random tick, max 1000
        // TODO this might not be random enough?
        _backup_ai_tick_thres = _rng() % (int(_latest_start + 0.5) + 1);
        return res;
    }

protected:
    // Backup AI.
    // Used when we want the default ai to play for a while and then TrainedAI can take over.
    std::unique_ptr<AI> _backup_ai;
    std::unique_ptr<AI> _main_ai;

    Tick _backup_ai_tick_thres = 0;
    std::mt19937 _rng;

    // Latest start of backup AI. When training, before each game starts,
    // we will sample a tick ~ Uniform(0, latest_start) and run backup AI
    // until that tick, then switch to NN-AI.
    int _latest_start = 0;

    // Decay of latest_start after each game.
    // After each game, latest_start is decayed by latest_start_decay.
    float _latest_start_decay = 0;

    static std::map<std::string, std::string> _parse(const std::string& args) {
        std::map<std::string, std::string> kvmap;
        for (const auto &item : CmdLineUtils::split(args, '|')) {
            std::vector<std::string> kv = CmdLineUtils::split(item, '/');
            if (kv.size() != 2) continue;
            kvmap.insert(std::make_pair(kv[0], kv[1]));
        }
        return kvmap;
    }

    void on_set_id() override {
        this->AI::on_set_id();
        if (_backup_ai != nullptr) _backup_ai->SetId(id());
        if (_main_ai != nullptr) _main_ai->SetId(id());
    }

    void on_set_state() override {
        this->AI::on_set_state();
        if (_backup_ai != nullptr) _backup_ai->SetState(s());
        if (_main_ai != nullptr) _main_ai->SetState(s());
    }

    bool on_act(Tick t, RTSMCAction *a, const std::atomic_bool *done) override {
        if (_backup_ai != nullptr && t < _backup_ai_tick_thres) {
            return _backup_ai->Act(t, a, done);
        } else {
            return _main_ai->Act(t, a, done);
        }
    }
};
