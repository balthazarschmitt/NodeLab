#pragma once

// The colour eyedropper: a Color param waiting for a pick from one of the image panels (click a
// pixel, or drag a rectangle to average an area). Started from the param's Pick button; App
// applies the pick and ends it. Esc or right-click cancels.
struct Eyedropper {
    int node = 0;  // node id in the graph being edited; 0 = inactive
    int param = -1;

    bool active() const { return node != 0; }
    bool is(int n, int p) const { return node == n && param == p; }
    void start(int n, int p) {
        node = n;
        param = p;
    }
    void toggle(int n, int p) {
        if (is(n, p)) cancel();
        else start(n, p);
    }
    void cancel() { *this = Eyedropper{}; }
};

inline Eyedropper& eyedropper() {
    static Eyedropper e;
    return e;
}
