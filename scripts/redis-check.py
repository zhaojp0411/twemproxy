import redis

r = redis.StrictRedis(host='localhost', port=22121, db=0)

# lrange
print [r.lrange('lfoo', 0, x) for x in range(1, 1000)]
print [r.lpush('lfoo', str(x)*32) for x in range(1, 1000)]
print [r.lrange('lfoo', 0, x) for x in range(1, 1000)]

# mget
print [r.set('foo' + str(x), str(x)*100) for x in range(1, 1000)]
keys = ['foo' + str(x) for x in range(1, 1000)]
print [r.mget(keys) for x in range(1, 1000)]

# del
print [r.set('foo' + str(x), str(x)*100) for x in range(1, 1000)]
keys = ['foo' + str(x) for x in range(1, 1000)]
print [r.delete(keys) for x in range(1, 1000)]

