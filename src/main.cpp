#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <deque>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float STEP_DT = 1.f / 240.f;

    using Inputs = std::vector<bool>;

    struct Snap {
        CheckpointObject* cp;
        bool down;
    };

    std::deque<Snap> g_ring;
    bool g_sim = false;          // simulando variantes
    bool g_diedInSim = false;    // la variante actual murió
    bool g_pending = false;      // hay una muerte retenida
    bool g_busy = false;         // evita reentradas mientras se corrige
    bool g_allowDeath = false;   // dejar pasar la muerte real
    PlayerObject* g_deadPlayer = nullptr;
    GameObject* g_deadObj = nullptr;

    // Ajustes
    bool cfgEnabled()   { return Mod::get()->getSettingValue<bool>("enabled"); }
    bool cfgPractice()  { return Mod::get()->getSettingValue<bool>("practice-only"); }
    bool cfgNotify()    { return Mod::get()->getSettingValue<bool>("notify"); }
    size_t cfgWindow()  { return static_cast<size_t>(Mod::get()->getSettingValue<int64_t>("window")); }
    int cfgShift()      { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-shift")); }
    size_t cfgExtra()   { return static_cast<size_t>(Mod::get()->getSettingValue<int64_t>("extra-steps")); }
    int cfgTries()      { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-tries")); }

    bool active(PlayLayer* pl) {
        if (!pl || !cfgEnabled()) return false;
        if (cfgPractice() && !pl->m_isPracticeMode) return false;
        return true;
    }

    bool holding(PlayLayer* pl) {
        return pl->m_player1->m_holdingButtons[1];
    }

    void clearRing() {
        for (auto& s : g_ring) s.cp->release();
        g_ring.clear();
    }

    // Variantes de los inputs, de menor a mayor cambio.
    std::vector<Inputs> makeVariants(Inputs const& in) {
        std::vector<Inputs> out;
        int n = static_cast<int>(in.size());
        int maxShift = cfgShift();

        std::vector<int> trans; // índices donde cambia el estado del botón
        for (int i = 1; i < n; i++) {
            if (in[i] != in[i - 1]) trans.push_back(i);
        }

        // 1) Mover los 2 últimos cambios (click o suelta) hasta +-maxShift
        for (int d = 1; d <= maxShift; d++) {
            for (int sign : {1, -1}) {
                int delta = sign * d;
                int count = 0;
                for (int k = static_cast<int>(trans.size()) - 1; k >= 0 && count < 2; k--, count++) {
                    int t = trans[k];
                    int nt = std::clamp(t + delta, 0, n);
                    bool val = in[t];
                    Inputs v = in;
                    if (nt > t) { for (int i = t; i < nt; i++) v[i] = !val; }
                    else        { for (int i = nt; i < t; i++) v[i] = val; }
                    if (v != in) out.push_back(std::move(v));
                }
            }
        }

        // 2) Quitar un click (el segmento presionado completo)
        for (int k = static_cast<int>(trans.size()) - 1; k >= 0; k--) {
            int t = trans[k];
            if (!in[t]) continue;
            int end = (k + 1 < static_cast<int>(trans.size())) ? trans[k + 1] : n;
            Inputs v = in;
            for (int i = t; i < end; i++) v[i] = false;
            out.push_back(std::move(v));
        }

        // 3) Agregar un click corto que no hiciste
        for (int s = n - 1; s >= 0; s--) {
            for (int len = 1; len <= 3; len++) {
                if (s + len > n) break;
                bool isFree = true;
                for (int i = s; i < s + len; i++) if (in[i]) isFree = false;
                if (!isFree) break;
                Inputs v = in;
                for (int i = s; i < s + len; i++) v[i] = true;
                out.push_back(std::move(v));
            }
        }

        return out;
    }

    // Restaura 'start' y simula 'total' pasos con los inputs dados.
    // Devuelve true si no murió.
    bool simulate(PlayLayer* pl, CheckpointObject* start, Inputs const& in, size_t total) {
        pl->loadFromCheckpoint(start);
        g_diedInSim = false;
        for (size_t i = 0; i < total; i++) {
            bool want = i < in.size() ? in[i] : in.back();
            if (holding(pl) != want) pl->handleButton(want, 1, true);
            pl->m_extraDelta = 0.f;
            pl->GJBaseGameLayer::update(STEP_DT);
            if (g_diedInSim) return false;
        }
        return true;
    }
}

class $modify(AssistBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (active(pl) && !g_sim && !g_pending && !g_busy && !pl->m_player1->m_isDead) {
            auto cp = pl->createCheckpoint();
            cp->retain();
            g_ring.push_back({ cp, holding(pl) });
            size_t win = cfgWindow();
            while (g_ring.size() > win) {
                g_ring.front().cp->release();
                g_ring.pop_front();
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(AssistPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (!active(this) || g_allowDeath || obj == m_anticheatSpike) {
            PlayLayer::destroyPlayer(p, obj);
            return;
        }
        if (g_sim) { g_diedInSim = true; return; }
        if (g_pending || g_busy) return;
        if (g_ring.size() < 2) {
            PlayLayer::destroyPlayer(p, obj);
            return;
        }
        // Retener la muerte; se intenta corregir al terminar el update
        g_pending = true;
        g_deadPlayer = p;
        g_deadObj = obj;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g_sim || !g_pending) return;
        g_pending = false;
        g_busy = true;

        bool fixed = tryFix();
        if (fixed) {
            if (cfgNotify()) {
                Notification::create("Click corregido", NotificationIcon::Success, 0.4f)->show();
            }
        } else {
            g_allowDeath = true;
            PlayLayer::destroyPlayer(g_deadPlayer, g_deadObj);
            g_allowDeath = false;
        }

        clearRing();
        g_busy = false;
    }

    bool tryFix() {
        Inputs in;
        for (auto& s : g_ring) in.push_back(s.down);
        size_t steps = g_ring.size();

        auto live = this->createCheckpoint();   // estado actual, por si no hay solución
        live->retain();

        g_sim = true;
        bool fixed = false;
        int tries = 0;
        int maxTries = cfgTries();
        size_t extra = cfgExtra();

        for (auto& v : makeVariants(in)) {
            if (++tries > maxTries) break;
            if (simulate(this, g_ring.front().cp, v, steps + extra)) {
                simulate(this, g_ring.front().cp, v, steps);   // dejar el juego en el punto de la muerte, ya corregido
                fixed = true;
                break;
            }
        }
        if (!fixed) this->loadFromCheckpoint(live);
        g_sim = false;
        live->release();
        return fixed;
    }

    void levelComplete() {
        if (g_sim) return;     // no completar el nivel dentro de una simulación
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        clearRing();
        g_pending = false;
        g_busy = false;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        clearRing();
        g_pending = false;
        g_busy = false;
        PlayLayer::onQuit();
    }
};
