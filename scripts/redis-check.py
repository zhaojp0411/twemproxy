import redis

r = redis.StrictRedis(host='localhost', port=22121, db=0)

print [r.lpush('lfoo', x) for x in range(1, 10000)]
print r.lrange('lfoo', 0, -1)
