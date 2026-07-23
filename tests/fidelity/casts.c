int castit(long v)
{
	int x;
	x = (int)v;
	return x;
}

int sizeit(void)
{
	int n;
	n = (int)sizeof(int);
	return n;
}

int castptr(int *p)
{
	int *q;
	q = (int *)p;
	return *q;
}
