// A line-by-line port of the parts of Pillow 11.3.0's quantizer the studio uses (see pillow_quant.h). The hash
// table, its resize rule (including _findPrime as written), the unstable list merge sort, the box heap and the
// distance tables are kept exactly, because each of them decides the palette's order or its rounding.
#include "pillow_quant.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace host::pillow {

namespace {

struct Pixel {
    uint8_t v[4];                                        // r, g, b, a (Quant.c's union Pixel)
};

// ---- QuantHash.c ----
struct HashNode {
    HashNode* next;
    Pixel key;
    uint32_t value;
};

constexpr uint32_t MIN_LENGTH = 11;
constexpr uint32_t RESIZE_FACTOR = 3;

inline uint32_t pixel_hash_rgb(uint32_t r, uint32_t g, uint32_t b) {   // PIXEL_HASH
    return (r * 463u) ^ ((g << 8) * 10069u) ^ ((b << 16) * 64997u);
}

// The pixel hash table of Quant.c (pixel_hash / pixel_cmp with the scale as user data).
struct HashTable {
    std::vector<HashNode*> table;
    uint32_t length = MIN_LENGTH, count = 0;
    uint32_t scale = 0;
    std::deque<HashNode> pool;

    HashTable() : table(MIN_LENGTH, nullptr) {}

    uint32_t hash(const Pixel& p) const {
        return pixel_hash_rgb(p.v[0] >> scale, p.v[1] >> scale, p.v[2] >> scale);
    }
    int cmp(const Pixel& a, const Pixel& b) const {
        uint32_t A = hash(a), B = hash(b);
        return A == B ? 0 : A < B ? -1 : 1;
    }
    HashNode* node() {
        pool.emplace_back();
        return &pool.back();
    }

    static uint32_t find_prime(uint32_t start, int dir) {
        static const int unit[] = {0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0};
        uint32_t t;
        while (start > 1) {
            if (!unit[start & 0x0f]) {
                start += dir;
                continue;
            }
            for (t = 2; t < std::sqrt((double)start); t++)
                if ((!start) % t)                        // as in QuantHash.c: never true
                    break;
            if (t >= std::sqrt((double)start))
                break;
            start += dir;
        }
        return start;
    }

    // _hashtable_insert_node; collide: rehash_collide (add the values) instead of replacing
    void insert_node(HashNode* node, bool resize_after, bool collide) {
        uint32_t hv = hash(node->key) % length;
        HashNode** n;
        for (n = &table[hv]; *n; n = &((*n)->next)) {
            HashNode* nv = *n;
            int i = cmp(nv->key, node->key);
            if (!i) {
                nv->key = node->key;
                if (collide)
                    nv->value += node->value;
                else
                    nv->value = node->value;
                return;
            } else if (i > 0) {
                break;
            }
        }
        node->next = *n;
        *n = node;
        count++;
        if (resize_after)
            resize();
    }

    void rehash(bool collide, uint32_t new_size) {
        std::vector<HashNode*> old = std::move(table);
        table.assign(new_size, nullptr);
        length = new_size;
        count = 0;
        for (HashNode* n : old)
            for (HashNode* nn; n; n = nn) {
                nn = n->next;
                insert_node(n, false, collide);
            }
    }

    void resize() {
        uint32_t old_size = length, new_size = length;
        if (count * RESIZE_FACTOR < length)
            new_size = find_prime(length / 2 - 1, -1);
        else if (length * RESIZE_FACTOR < count)
            new_size = find_prime(length * 2 + 1, +1);
        if (new_size < MIN_LENGTH)
            new_size = old_size;
        if (new_size != old_size)
            rehash(false, new_size);
    }

    // hashtable_insert_or_update_computed with new_count_func / exists_count_func
    void count_pixel(const Pixel& key) {
        uint32_t hv = hash(key) % length;
        HashNode** n;
        for (n = &table[hv]; *n; n = &((*n)->next)) {
            HashNode* nv = *n;
            int i = cmp(nv->key, key);
            if (!i) {
                nv->value += 1;
                return;
            } else if (i > 0) {
                break;
            }
        }
        HashNode* t = node();
        t->key = key;
        t->next = *n;
        *n = t;
        t->value = 1;
        count++;
        resize();
    }

    // hashtable_insert (_hashtable_insert with resize, no update)
    void insert(const Pixel& key, uint32_t val) {
        uint32_t hv = hash(key) % length;
        HashNode** n;
        for (n = &table[hv]; *n; n = &((*n)->next)) {
            HashNode* nv = *n;
            int i = cmp(nv->key, key);
            if (!i) {
                nv->value = val;
                return;
            } else if (i > 0) {
                break;
            }
        }
        HashNode* t = node();
        t->next = *n;
        *n = t;
        t->key = key;
        t->value = val;
        count++;
        resize();
    }

    bool lookup(const Pixel& key, uint32_t* valp) const {
        uint32_t hv = hash(key) % length;
        for (HashNode* n = table[hv]; n; n = n->next) {
            int i = cmp(n->key, key);
            if (!i) {
                *valp = n->value;
                return true;
            } else if (i > 0) {
                break;
            }
        }
        return false;
    }
};

constexpr uint32_t MAX_HASH_ENTRIES = 65536;

// ---- Quant.c: median cut ----
struct PixelList {
    PixelList *next[3], *prev[3];
    Pixel p;
    unsigned flag;
    int count;
};

struct BoxNode {
    BoxNode *l, *r;
    PixelList *head[3], *tail[3];
    int axis;
    int volume;
    uint32_t pixelCount;
};

inline uint32_t distsqr(const Pixel& a, const Pixel& b) {
    int dr = (int)a.v[0] - (int)b.v[0], dg = (int)a.v[1] - (int)b.v[1], db = (int)a.v[2] - (int)b.v[2];
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

int compute_box_volume(BoxNode* b) {
    if (b->volume >= 0)
        return b->volume;
    if (!b->head[0]) {
        b->volume = 0;
    } else {
        unsigned char rh = b->head[0]->p.v[0], rl = b->tail[0]->p.v[0];
        unsigned char gh = b->head[1]->p.v[1], gl = b->tail[1]->p.v[1];
        unsigned char bh = b->head[2]->p.v[2], bl = b->tail[2]->p.v[2];
        b->volume = (rh - rl + 1) * (gh - gl + 1) * (bh - bl + 1);
    }
    return b->volume;
}

PixelList* mergesort_pixels(PixelList* head, int i) {
    PixelList *c, *t, *a, *b, *p;
    if (!head || !head->next[i]) {
        if (head) {
            head->next[i] = nullptr;
            head->prev[i] = nullptr;
        }
        return head;
    }
    for (c = t = head; c && t; c = c->next[i], t = (t->next[i]) ? t->next[i]->next[i] : nullptr)
        ;
    if (c) {
        if (c->prev[i])
            c->prev[i]->next[i] = nullptr;
        c->prev[i] = nullptr;
    }
    a = mergesort_pixels(head, i);
    b = mergesort_pixels(c, i);
    head = nullptr;
    p = nullptr;
    while (a && b) {
        if (a->p.v[i] > b->p.v[i]) {
            c = a;
            a = a->next[i];
        } else {
            c = b;
            b = b->next[i];
        }
        c->prev[i] = p;
        c->next[i] = nullptr;
        if (p)
            p->next[i] = c;
        p = c;
        if (!head)
            head = c;
    }
    if (a) {
        c->next[i] = a;
        a->prev[i] = c;
    } else if (b) {
        c->next[i] = b;
        b->prev[i] = c;
    }
    return head;
}

void splitlists(PixelList* h[3], PixelList* t[3], PixelList* nh[2][3], PixelList* nt[2][3], uint32_t nCount[2],
                int axis, uint32_t pixelCount) {
    uint32_t left;
    PixelList *l, *r, *c, *n;
    int nRight;
    int splitColourVal;

    nCount[0] = nCount[1] = 0;
    nRight = 0;
    for (left = 0, c = h[axis]; c;) {
        left = left + c->count;
        nCount[0] += c->count;
        c->flag = 0;
        c = c->next[axis];
        if (left * 2 > pixelCount)
            break;
    }
    if (c) {
        splitColourVal = c->prev[axis]->p.v[axis];
        for (; c; c = c->next[axis]) {
            if (splitColourVal != c->p.v[axis])
                break;
            c->flag = 0;
            nCount[0] += c->count;
        }
    }
    for (; c; c = c->next[axis]) {
        c->flag = 1;
        nRight++;
        nCount[1] += c->count;
    }
    if (!nRight) {
        for (c = t[axis], splitColourVal = t[axis]->p.v[axis]; c; c = c->prev[axis]) {
            if (splitColourVal != c->p.v[axis])
                break;
            c->flag = 1;
            nRight++;
            nCount[0] -= c->count;
            nCount[1] += c->count;
        }
    }
    for (int i = 0; i < 3; i++) {
        l = r = nullptr;
        nh[0][i] = nt[0][i] = nullptr;
        nh[1][i] = nt[1][i] = nullptr;
        for (c = h[i]; c; c = n) {
            n = c->next[i];
            if (c->flag) {
                if (r)
                    r->next[i] = c;
                else
                    nh[1][i] = c;
                c->prev[i] = r;
                r = c;
            } else {
                if (l)
                    l->next[i] = c;
                else
                    nh[0][i] = c;
                c->prev[i] = l;
                l = c;
            }
        }
        if (l)
            l->next[i] = nullptr;
        if (r)
            r->next[i] = nullptr;
        nt[0][i] = l;
        nt[1][i] = r;
    }
}

struct Boxes {
    std::deque<BoxNode> pool;
    BoxNode* make() {
        pool.emplace_back();
        return &pool.back();
    }
};

void split(BoxNode* node, Boxes& boxes) {
    unsigned char rl, rh, gl, gh, bl, bh;
    int f[3];
    PixelList* heads[2][3];
    PixelList* tails[2][3];
    uint32_t newCounts[2];

    rh = node->head[0]->p.v[0];
    rl = node->tail[0]->p.v[0];
    gh = node->head[1]->p.v[1];
    gl = node->tail[1]->p.v[1];
    bh = node->head[2]->p.v[2];
    bl = node->tail[2]->p.v[2];
    f[0] = (rh - rl) * 77;
    f[1] = (gh - gl) * 150;
    f[2] = (bh - bl) * 29;
    int best = f[0], axis = 0;
    for (int i = 1; i < 3; i++)
        if (best < f[i]) {
            best = f[i];
            axis = i;
        }
    node->axis = axis;
    splitlists(node->head, node->tail, heads, tails, newCounts, axis, node->pixelCount);
    BoxNode* left = boxes.make();
    BoxNode* right = boxes.make();
    for (int i = 0; i < 3; i++) {
        left->head[i] = heads[0][i];
        left->tail[i] = tails[0][i];
        right->head[i] = heads[1][i];
        right->tail[i] = tails[1][i];
        node->head[i] = nullptr;
        node->tail[i] = nullptr;
    }
    left->l = left->r = nullptr;
    right->l = right->r = nullptr;
    left->axis = right->axis = -1;
    left->volume = right->volume = -1;
    left->pixelCount = newCounts[0];
    right->pixelCount = newCounts[1];
    node->l = left;
    node->r = right;
}

// QuantHeap.c with box_heap_cmp (the box with the most pixels on top)
struct BoxHeap {
    std::vector<BoxNode*> heap{nullptr};
    static int cf(const BoxNode* a, const BoxNode* b) { return (int)a->pixelCount - (int)b->pixelCount; }
    bool remove(BoxNode** r) {
        if (heap.size() == 1)
            return false;
        unsigned heapcount = (unsigned)heap.size() - 1;
        *r = heap[1];
        BoxNode* v = heap[heapcount--];
        unsigned k, l;
        for (k = 1; k * 2 <= heapcount; k = l) {
            l = k * 2;
            if (l < heapcount && cf(heap[l], heap[l + 1]) < 0)
                l++;
            if (cf(v, heap[l]) > 0)
                break;
            heap[k] = heap[l];
        }
        heap[k] = v;
        heap.pop_back();
        return true;
    }
    void add(BoxNode* val) {
        heap.push_back(nullptr);
        int k = (int)heap.size() - 1;
        while (k != 1) {
            if (cf(val, heap[k / 2]) <= 0)
                break;
            heap[k] = heap[k / 2];
            k >>= 1;
        }
        heap[k] = val;
    }
};

BoxNode* median_cut(PixelList* hl[3], uint32_t imPixelCount, int nPixels, Boxes& boxes) {
    BoxHeap h;
    BoxNode* root = boxes.make();
    for (int i = 0; i < 3; i++) {
        PixelList* tl;
        for (tl = hl[i]; tl && tl->next[i]; tl = tl->next[i])
            ;
        root->head[i] = hl[i];
        root->tail[i] = tl;
    }
    root->l = root->r = nullptr;
    root->axis = -1;
    root->volume = -1;
    root->pixelCount = imPixelCount;
    h.add(root);
    BoxNode* thisNode;
    while (--nPixels) {
        do {
            if (!h.remove(&thisNode))
                return root;
        } while (compute_box_volume(thisNode) == 1);
        split(thisNode, boxes);
        h.add(thisNode->l);
        h.add(thisNode->r);
    }
    return root;
}

bool annotate_hash_table(BoxNode* n, HashTable& h, uint32_t* box) {
    if (n->l && n->r)
        return annotate_hash_table(n->l, h, box) && annotate_hash_table(n->r, h, box);
    if (n->l || n->r)
        return false;
    for (PixelList* p = n->head[0]; p; p = p->next[0]) {
        Pixel q;                                         // PIXEL_UNSCALE (q's alpha is not used by the table)
        q.v[0] = (uint8_t)(p->p.v[0] << h.scale);
        q.v[1] = (uint8_t)(p->p.v[1] << h.scale);
        q.v[2] = (uint8_t)(p->p.v[2] << h.scale);
        q.v[3] = 0;
        h.insert(q, *box);
    }
    if (n->head[0])
        (*box)++;
    return true;
}

}  // namespace

Quantized quantize_mediancut(const uint8_t* px, size_t npixels, size_t stride, int colors, bool want_index) {
    if (colors < 1 || colors > 256)
        throw std::runtime_error("bad number of colors");
    Quantized out;
    if (!npixels)
        return out;
    std::vector<Pixel> pixelData(npixels);
    for (size_t i = 0; i < npixels; i++)
        pixelData[i] = {{px[i * stride], px[i * stride + 1], px[i * stride + 2], 255}};
    uint32_t nPixels = (uint32_t)npixels;

    // create_pixel_hash
    HashTable h;
    for (uint32_t i = 0; i < nPixels; i++) {
        h.count_pixel(pixelData[i]);
        while (h.count > MAX_HASH_ENTRIES) {
            h.scale++;
            h.rehash(true, h.length);
        }
    }

    // hash_to_list: each entry pushed on the front of the three lists, in table order
    std::deque<PixelList> lists;
    PixelList* hl[3] = {nullptr, nullptr, nullptr};
    for (uint32_t x = 0; x < h.length; x++)
        for (HashNode* n = h.table[x]; n; n = n->next) {
            lists.emplace_back();
            PixelList* p = &lists.back();
            p->flag = 0;
            p->p.v[0] = (uint8_t)(n->key.v[0] >> h.scale);
            p->p.v[1] = (uint8_t)(n->key.v[1] >> h.scale);
            p->p.v[2] = (uint8_t)(n->key.v[2] >> h.scale);
            p->p.v[3] = 0;
            p->count = (int)n->value;
            for (int i = 0; i < 3; i++) {
                p->next[i] = hl[i];
                p->prev[i] = nullptr;
                if (hl[i])
                    hl[i]->prev[i] = p;
                hl[i] = p;
            }
        }
    if (!hl[0])
        throw std::runtime_error("quantization error");
    for (int i = 0; i < 3; i++)
        hl[i] = mergesort_pixels(hl[i], i);

    Boxes boxes;
    BoxNode* root = median_cut(hl, nPixels, colors, boxes);
    uint32_t nPaletteEntries = 0;
    annotate_hash_table(root, h, &nPaletteEntries);

    // compute_palette_from_median_cut
    std::vector<uint32_t> count(nPaletteEntries, 0), avg[3];
    for (auto& a : avg)
        a.assign(nPaletteEntries, 0);
    for (uint32_t i = 0; i < nPixels; i++) {
        uint32_t e;
        if (!h.lookup(pixelData[i], &e) || e >= nPaletteEntries)
            throw std::runtime_error("quantization error");
        for (int c = 0; c < 3; c++)
            avg[c][e] += pixelData[i].v[c];
        count[e]++;
    }
    std::vector<Pixel> p(nPaletteEntries);
    for (uint32_t i = 0; i < nPaletteEntries; i++) {
        for (int c = 0; c < 3; c++)
            p[i].v[c] = (uint8_t)(int)(.5 + (double)avg[c][i] / (double)count[i]);
        p[i].v[3] = 0;
    }
    out.palette.resize(nPaletteEntries);
    for (uint32_t i = 0; i < nPaletteEntries; i++)
        out.palette[i] = {p[i].v[0], p[i].v[1], p[i].v[2]};
    if (!want_index)
        return out;

    // build_distance_tables: per entry, the other entries by distance (ties: lower index first)
    uint32_t nE = nPaletteEntries;
    std::vector<uint32_t> avgDist((size_t)nE * nE);
    std::vector<uint32_t> sortKey((size_t)nE * nE);      // column of the j-th nearest entry in row i
    for (uint32_t i = 0; i < nE; i++) {
        avgDist[(size_t)i * nE + i] = 0;
        for (uint32_t j = 0; j < i; j++)
            avgDist[(size_t)j * nE + i] = avgDist[(size_t)i * nE + j] = distsqr(p[i], p[j]);
    }
    for (uint32_t i = 0; i < nE; i++) {
        uint32_t* row = &sortKey[(size_t)i * nE];
        const uint32_t* d = &avgDist[(size_t)i * nE];
        for (uint32_t j = 0; j < nE; j++)
            row[j] = j;
        std::sort(row, row + nE, [&](uint32_t a, uint32_t b) { return d[a] != d[b] ? d[a] < d[b] : a < b; });
    }

    // map_image_pixels_from_median_box (its exact-colour cache h2 only memoizes)
    out.index.resize(npixels);
    std::unordered_map<uint32_t, uint32_t> h2;
    for (uint32_t i = 0; i < nPixels; i++) {
        const Pixel& px_i = pixelData[i];
        uint32_t key = (uint32_t)px_i.v[0] << 16 | (uint32_t)px_i.v[1] << 8 | px_i.v[2];
        auto it = h2.find(key);
        if (it != h2.end()) {
            out.index[i] = (uint8_t)it->second;
            continue;
        }
        uint32_t pixelVal;
        if (!h.lookup(px_i, &pixelVal))
            throw std::runtime_error("quantization error");
        uint32_t initialdist = distsqr(p[pixelVal], px_i);
        uint32_t bestdist = initialdist, bestmatch = pixelVal;
        initialdist <<= 2;
        const uint32_t* aDSK = &sortKey[(size_t)pixelVal * nE];
        const uint32_t* aD = &avgDist[(size_t)pixelVal * nE];
        for (uint32_t j = 0; j < nE; j++) {
            uint32_t idx = aDSK[j];
            if (aD[idx] <= initialdist) {
                uint32_t dist = distsqr(p[idx], px_i);
                if (dist < bestdist) {
                    bestdist = dist;
                    bestmatch = idx;
                }
            } else {
                break;
            }
        }
        out.index[i] = (uint8_t)bestmatch;
        h2.emplace(key, bestmatch);
    }
    return out;
}

// ---- Palette.c: the palette cache ----
PaletteMapper::PaletteMapper(const std::vector<Rgb>& palette) : pal_(palette), cache_(64 * 64 * 64, 0x100) {}

uint8_t PaletteMapper::index(int r, int g, int b) {
    int16_t& c = cache_[(r >> 2) + (g >> 2) * 64 + (b >> 2) * 64 * 64];
    if (c == 0x100)
        update(r, g, b);
    return (uint8_t)c;
}

// ImagingPaletteCacheUpdate: fills the 8 x 8 x 8 cache slots of the 32-value box around (r, g, b)
void PaletteMapper::update(int r, int g, int b) {
    constexpr int BOX = 8, STEP = 4;
    int size = (int)pal_.size();
    unsigned int dmin[256], dmax;
    unsigned int d[BOX * BOX * BOX];
    uint8_t c[BOX * BOX * BOX] = {};
    int r0 = r & 0xe0, r1 = r0 + 0x1f, rc = (r0 + r1) / 2;
    int g0 = g & 0xe0, g1 = g0 + 0x1f, gc = (g0 + g1) / 2;
    int b0 = b & 0xe0, b1 = b0 + 0x1f, bc = (b0 + b1) / 2;
    auto dist = [](int a, int b) { return (unsigned int)((a - b) * (a - b)); };
    dmax = ~0u;
    for (int i = 0; i < size; i++) {
        int pr = pal_[i][0], pg = pal_[i][1], pb = pal_[i][2];
        unsigned int tmin = (pr < r0) ? dist(pr, r0) : (pr > r1) ? dist(pr, r1) : 0;
        unsigned int tmax = (pr <= rc) ? dist(pr, r1) : dist(pr, r0);
        tmin += (pg < g0) ? dist(pg, g0) : (pg > g1) ? dist(pg, g1) : 0;
        tmax += (pg <= gc) ? dist(pg, g1) : dist(pg, g0);
        tmin += (pb < b0) ? dist(pb, b0) : (pb > b1) ? dist(pb, b1) : 0;
        tmax += (pb <= bc) ? dist(pb, b1) : dist(pb, b0);
        dmin[i] = tmin;
        if (tmax < dmax)
            dmax = tmax;
    }
    for (auto& v : d)
        v = ~0u;
    for (int i = 0; i < size; i++) {
        if (dmin[i] > dmax)
            continue;
        int ri = r0 - pal_[i][0], gi = g0 - pal_[i][1], bi = b0 - pal_[i][2];
        int rd = ri * ri + gi * gi + bi * bi;
        ri = ri * (2 * STEP) + STEP * STEP;
        gi = gi * (2 * STEP) + STEP * STEP;
        bi = bi * (2 * STEP) + STEP * STEP;
        int rx = ri, j = 0;
        for (int rr = 0; rr < BOX; rr++) {
            int gd = rd, gx = gi;
            for (int gg = 0; gg < BOX; gg++) {
                int bd = gd, bx = bi;
                for (int bb = 0; bb < BOX; bb++) {
                    if ((unsigned int)bd < d[j]) {
                        d[j] = (unsigned int)bd;
                        c[j] = (uint8_t)i;
                    }
                    bd += bx;
                    bx += 2 * STEP * STEP;
                    j++;
                }
                gd += gx;
                gx += 2 * STEP * STEP;
            }
            rd += rx;
            rx += 2 * STEP * STEP;
        }
    }
    int j = 0;
    for (int rr = r0; rr < r1; rr += 4)
        for (int gg = g0; gg < g1; gg += 4)
            for (int bb = b0; bb < b1; bb += 4)
                cache_[(rr >> 2) + (gg >> 2) * 64 + (bb >> 2) * 64 * 64] = c[j++];
}

}  // namespace host::pillow
