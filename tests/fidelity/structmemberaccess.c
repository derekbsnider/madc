/* structmemberaccess.c — `.` vs `->` selection across object shapes.
 *
 * Regression for the bug where `m_data[i].field` (m_data a POINTER, so
 * m_data[i] is a struct VALUE) was emitted as `m_data[i]->field`. gcc rejects
 * `->` on a struct value, so the emitted C fails the fidelity gate (EMIT-BAD-C)
 * — this reducer would have caught it. Covers every object shape:
 *   s.f          struct value           -> .
 *   ps->f        pointer                 -> ->
 *   arr[i].f     array element (value)   -> .
 *   pp[i].f      pointer subscript value -> .   (the bug: was ->)
 *   ppp[i]->f    array/ptr of pointers   -> ->
 */

struct cell { int x; int y; };

int sum_value(struct cell s)        { return s.x + s.y; }
int sum_ptr(struct cell *ps)        { return ps->x + ps->y; }
int sum_arr(struct cell arr[4])     { int i, t = 0; for (i = 0; i < 4; i++) t += arr[i].x; return t; }
int sum_pptr(struct cell *p, int n) { int i, t = 0; for (i = 0; i < n; i++) t += p[i].x; return t; }
int sum_ptrarr(struct cell **pp, int n) { int i, t = 0; for (i = 0; i < n; i++) t += pp[i]->x; return t; }

int main(void)
{
	struct cell a[4] = { {1,2}, {3,4}, {5,6}, {7,8} };
	struct cell *ptrs[4]; int i;
	for (i = 0; i < 4; i++) ptrs[i] = &a[i];
	return sum_value(a[0]) + sum_ptr(&a[1]) + sum_arr(a)
	     + sum_pptr(a, 4) + sum_ptrarr(ptrs, 4);
}
