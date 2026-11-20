// aqui é só o porteiro: manda tudo pro Vulkan ou pro DX11 (detalhes em docs/renderizadores.md)
#include "gxm_vk.h"
#include "trophy_banner.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstring>
#include <string>
#ifdef __ANDROID__
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#endif

namespace vt {
void logf(const char *fmt, ...); // core.cpp
}

namespace gvk {
namespace vkb {
#include "gxm_api.inc"
}
namespace dx11 {
#include "gxm_api.inc"
}

namespace {
Backend g_backend = Backend::vulkan;
std::string g_want = "auto";
} // namespace

void select_backend(const char *name) { g_want = name && *name ? name : "auto"; }
Backend backend() { return g_backend; }
const char *backend_name() { return g_backend == Backend::d3d11 ? "Direct3D 11" : "Vulkan"; }

#ifdef __ANDROID__
// ------------------------------------------------------------------ gravação num thread próprio (celular)
// O HLE chamava o Vulkan no thread do jogo: cada desenho era gravado ali mesmo (2 a 7 ms por quadro numa GPU de
// celular), e o jogo só seguia depois. Com a fila, o thread do jogo só copia o que o desenho lê (vértices, índices,
// uniforms; shaders e texturas vão por ponteiro compartilhado) e segue; um thread de gravação executa as chamadas
// na mesma ordem. O jogo fica no máximo um quadro à frente (o present espera o anterior sair). As perguntas que o
// HLE faz na hora (é alvo? é profundidade?) respondem por uma sombra mantida aqui. VITART_VK_THREAD=0 desliga.
namespace fila {
std::mutex m;
std::condition_variable cv_tem, cv_vazia, cv_present;
std::deque<std::function<void()>> q;
bool ocupado = false, ligada = false, parar = false, dormindo = false;
uint32_t presents_na_fila = 0;
std::thread t;
std::mutex m_sombra;
std::unordered_set<uint32_t> alvos, profundidades;

void rodar() {
    vt::pin_fast_cores(); // thread de gravação do Vulkan nos núcleos rápidos (ver vitart.h)
    for (;;) {
        std::function<void()> f;
        {
            std::unique_lock<std::mutex> l(m);
            dormindo = true;
            cv_tem.wait(l, [] { return parar || !q.empty(); });
            dormindo = false;
            if (q.empty()) return;
            f = std::move(q.front());
            q.pop_front();
            ocupado = true;
        }
        f();
        {
            std::lock_guard<std::mutex> l(m);
            ocupado = false;
            if (q.empty()) cv_vazia.notify_all();
        }
    }
}
void por(std::function<void()> f) {
    bool acordar;
    {
        std::lock_guard<std::mutex> l(m);
        q.push_back(std::move(f));
        acordar = dormindo; // acordar custa uma chamada ao sistema: só se o thread de gravação está parado esperando
    }
    if (acordar) cv_tem.notify_one();
}
// espera tudo o que foi pedido ser executado (para as leituras que precisam do estado em dia)
void esvaziar() {
    if (!ligada) return;
    std::unique_lock<std::mutex> l(m);
    cv_vazia.wait(l, [] { return q.empty() && !ocupado; });
}
void iniciar() {
    const char *e = getenv("VITART_VK_THREAD");
    if (e && *e == '0') return;
    ligada = true;
    t = std::thread(rodar);
    vt::logf("vitart: gravação do Vulkan num thread próprio (VITART_VK_THREAD=0 desliga)");
}
void encerrar() {
    if (!ligada) return;
    esvaziar();
    {
        std::lock_guard<std::mutex> l(m);
        parar = true;
    }
    cv_tem.notify_all();
    if (t.joinable()) t.join();
    ligada = false;
}
// cópia do que o desenho lê: o HLE reaproveita os buffers e o jogo reescreve os seus depois que a chamada volta
struct DesenhoNaFila {
    Draw d;
    std::vector<uint8_t> dados;
};
std::shared_ptr<DesenhoNaFila> copiar(const Draw &src) {
    auto q = std::make_shared<DesenhoNaFila>();
    q->d = src;
    Draw &d = q->d;
    size_t total = 0;
    auto soma = [&](size_t n) { total += (n + 15) & ~size_t(15); };
    const bool cru = !d.raw_attrs.empty();
    for (const VertexStream &st : d.raw_streams) soma(st.size);
    if (!cru && d.attrs) soma((size_t)d.attr_count * d.vertex_count * 16);
    if (d.index_data) soma((size_t)d.index_count * d.index_size);
    else if (d.indices) soma((size_t)d.index_count * 4);
    if (d.vert_uniforms) soma(d.vert_uniforms_size);
    if (d.frag_uniforms) soma(d.frag_uniforms_size);
    q->dados.resize(total);
    size_t at = 0;
    auto poe = [&](const void *p, size_t n) -> const uint8_t * {
        uint8_t *o = q->dados.data() + at;
        if (p && n) std::memcpy(o, p, n);
        at += (n + 15) & ~size_t(15);
        return o;
    };
    for (VertexStream &st : d.raw_streams)
        if (st.data) st.data = poe(st.data, st.size);
    if (!cru && d.attrs) d.attrs = reinterpret_cast<const float *>(poe(d.attrs, (size_t)d.attr_count * d.vertex_count * 16));
    if (d.index_data) d.index_data = poe(d.index_data, (size_t)d.index_count * d.index_size);
    else if (d.indices) d.indices = reinterpret_cast<const uint32_t *>(poe(d.indices, (size_t)d.index_count * 4));
    if (d.vert_uniforms) d.vert_uniforms = poe(d.vert_uniforms, d.vert_uniforms_size);
    if (d.frag_uniforms) d.frag_uniforms = poe(d.frag_uniforms, d.frag_uniforms_size);
    return q;
}
} // namespace fila

// no celular é Vulkan e pronto
bool init(bool validation, bool window) {
    g_backend = Backend::vulkan;
    if (!vkb::init(validation, window)) return false;
    fila::iniciar();
    return true;
}
#define TO(f, ...) vkb::f(__VA_ARGS__)
// chamada que muda estado: vai para a fila (com a fila ligada), senão direto
#define FILA(chamada)                                  \
    do {                                               \
        if (fila::ligada) {                            \
            fila::por([=] { chamada; });               \
            return;                                    \
        }                                              \
    } while (0)
#else
// tenta o Vulkan primeiro; se o PC torcer o nariz, vai de DX11 mesmo
bool init(bool validation, bool window) {
    if (g_want != "d3d11") {
        if (LoadLibraryA("vulkan-1.dll")) {
            g_backend = Backend::vulkan;
            if (vkb::init(validation, window)) return true;
            if (g_want == "vulkan") return false;
            vt::logf("vitart: Vulkan não iniciou (%s); usando o Direct3D 11", vkb::last_error());
        } else {
            if (g_want == "vulkan") return false;
            vt::logf("vitart: sem driver Vulkan (vulkan-1.dll); usando o Direct3D 11");
        }
    }
    g_backend = Backend::d3d11;
    return dx11::init(validation, window);
}

// daqui pra baixo é tudo repasse, nada de emocionante
#define TO(f, ...) (g_backend == Backend::d3d11 ? dx11::f(__VA_ARGS__) : vkb::f(__VA_ARGS__))
#endif

bool vertex_format_ok(uint32_t fmt, uint32_t comps) { return TO(vertex_format_ok, fmt, comps); }
bool present(uint32_t addr) {
#ifdef __ANDROID__
    if (fila::ligada) {
        {   // no máximo um quadro à frente: espera o present anterior sair da fila
            std::unique_lock<std::mutex> l(fila::m);
            fila::cv_present.wait(l, [] { return fila::presents_na_fila == 0; });
            fila::presents_na_fila++;
        }
        fila::por([addr] {
            vkb::present(addr);
            {
                std::lock_guard<std::mutex> l(fila::m);
                fila::presents_na_fila--;
            }
            fila::cv_present.notify_all();
        });
        return true;
    }
#endif
    return TO(present, addr);
}
bool window_closed() { return TO(window_closed); }
bool window_rect(int &x, int &y, int &w, int &h) { return TO(window_rect, x, y, w, h); }
bool pointer(float &nx, float &ny) { return TO(pointer, nx, ny); }
bool has_focus() { return TO(has_focus); }
void notify(const std::string &text) {
#ifdef __ANDROID__
    FILA(vkb::notify(text));
#endif
    TO(notify, text);
}
bool is_target(uint32_t addr) {
#ifdef __ANDROID__
    if (fila::ligada) {
        std::lock_guard<std::mutex> l(fila::m_sombra);
        return fila::alvos.count(addr) != 0;
    }
#endif
    return TO(is_target, addr);
}
bool is_depth(uint32_t addr) {
#ifdef __ANDROID__
    if (fila::ligada) {
        std::lock_guard<std::mutex> l(fila::m_sombra);
        return fila::profundidades.count(addr) != 0;
    }
#endif
    return TO(is_depth, addr);
}
void begin_scene(uint32_t color_addr, uint8_t stencil) {
#ifdef __ANDROID__
    FILA(vkb::begin_scene(color_addr, stencil));
#endif
    TO(begin_scene, color_addr, stencil);
}
void shutdown() {
#ifdef __ANDROID__
    fila::encerrar();
#endif
    TO(shutdown);
}
uint32_t render_scale() { return TO(render_scale); }
uint32_t auto_render_scale() { return TO(auto_render_scale); }
bool set_render_scale(uint32_t s) {
#ifdef __ANDROID__
    if (fila::ligada) {
        fila::por([s] { vkb::set_render_scale(s); });
        return true;
    }
#endif
    return TO(set_render_scale, s);
}
float aspect_widen() { return TO(aspect_widen); }
bool set_aspect_widen(float w) {
#ifdef __ANDROID__
    if (fila::ligada) {
        fila::por([w] { vkb::set_aspect_widen(w); });
        return true;
    }
#endif
    return TO(set_aspect_widen, w);
}
void set_fps_counter(bool on) {
#ifdef __ANDROID__
    FILA(vkb::set_fps_counter(on));
#endif
    TO(set_fps_counter, on);
}
void draw_enemy_bars(uint32_t color_addr, const EnemyBar *bars, uint32_t n) {
#ifdef __ANDROID__
    if (fila::ligada) {
        std::vector<EnemyBar> v(bars, bars + n);
        fila::por([color_addr, v] { vkb::draw_enemy_bars(color_addr, v.data(), (uint32_t)v.size()); });
        return;
    }
#endif
    TO(draw_enemy_bars, color_addr, bars, n);
}
uint32_t measured_fps() { return TO(measured_fps); }
void set_borderless(bool on) { TO(set_borderless, on); }
void set_vsync(bool on, bool fifo) {
#ifdef __ANDROID__
    FILA(vkb::set_vsync(on, fifo));
#endif
    TO(set_vsync, on, fifo);
}
uint32_t last_frame_depth_writes() { return TO(last_frame_depth_writes); }
void set_post_bypass(bool on) {
#ifdef __ANDROID__
    FILA(vkb::set_post_bypass(on));
#endif
    TO(set_post_bypass, on);
}
void post_stats(uint32_t out[6]) { TO(post_stats, out); }
void post_focus_values(float out[4]) { TO(post_focus_values, out); }
void set_video_frame(const uint8_t *rgba, uint32_t w, uint32_t h) {
#ifdef __ANDROID__
    if (fila::ligada) {
        auto px = std::make_shared<std::vector<uint8_t>>(rgba, rgba + (size_t)w * h * 4);
        fila::por([px, w, h] {
            // segura o quadro no thread de gravação até o próximo chegar (o present seguinte, na mesma fila, o lê)
            static std::shared_ptr<std::vector<uint8_t>> atual;
            atual = px;
            vkb::set_video_frame(atual->data(), w, h);
        });
        return;
    }
#endif
    TO(set_video_frame, rgba, w, h);
}
void notify_trophy(const TrophyBanner &t) {
#ifdef __ANDROID__
    if (fila::ligada) {
        fila::por([t] { vkb::notify_trophy(t); });
        return;
    }
#endif
    TO(notify_trophy, t);
}
bool is_borderless() { return TO(is_borderless); }
bool draw(const Draw &d) {
#ifdef __ANDROID__
    if (fila::ligada) {
        {
            std::lock_guard<std::mutex> l(fila::m_sombra);
            fila::alvos.insert(d.target_addr);
            if (d.depth_addr) fila::profundidades.insert(d.depth_addr);
        }
        auto q = fila::copiar(d);
        fila::por([q] {
            if (!vkb::draw(q->d)) vt::logf("vitart: desenho Vulkan falhou: %s", vkb::last_error());
        });
        return true;
    }
#endif
    return TO(draw, d);
}
void end_frame() {
#ifdef __ANDROID__
    FILA(vkb::end_frame());
#endif
    TO(end_frame);
}
bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
#ifdef __ANDROID__
    fila::esvaziar();
#endif
    return TO(read_target, addr, rgba, w, h);
}
std::vector<uint32_t> targets() {
#ifdef __ANDROID__
    fila::esvaziar();
#endif
    return TO(targets);
}
const char *last_error() { return TO(last_error); }
const std::vector<float> &debug_vs_output() {
#ifdef __ANDROID__
    fila::esvaziar();
#endif
    return TO(debug_vs_output);
}
Stats take_stats() { return TO(take_stats); }
Stats peek_stats() { return TO(peek_stats); }

} // namespace gvk
