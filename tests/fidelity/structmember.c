struct point
{
	int x;
	int y;
};

int getx(struct point *p)
{
	return p->x;
}

int sety(void)
{
	struct point pt;
	pt.y = 7;
	return pt.y;
}
