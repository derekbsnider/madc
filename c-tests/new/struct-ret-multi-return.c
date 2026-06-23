struct Node {
  int key;
};

struct Holder {
  struct Node *rightmost;
  int count;
};

struct PairPtr {
  struct Node *first;
  struct Node *second;
};

void PairPtr_ctor (struct PairPtr *p, struct Node **x, struct Node **y) {
  p->first = *x;
  p->second = *y;
}

struct Node **rightmost (struct Holder *h) { return &h->rightmost; }

struct PairPtr fallback (struct Holder *h) {
  struct PairPtr p;
  PairPtr_ctor (&p, &h->rightmost, &h->rightmost);
  return p;
}

struct PairPtr choose (struct Holder *h, int key) {
  if (h->count > 0 && h->rightmost->key < key) {
    struct PairPtr p;
    struct Node *zero;
    zero = 0;
    PairPtr_ctor (&p, &zero, &(*rightmost (h)));
    return p;
  } else {
    struct PairPtr q;
    q = fallback (h);
    return q;
  }
}

int main (void) {
  struct Node a;
  struct Holder h;
  struct PairPtr p;
  a.key = 1;
  h.rightmost = &a;
  h.count = 1;
  p = choose (&h, 5);
  return !(p.first == 0 && p.second == &a);
}
