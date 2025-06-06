-- Tags: stateful, distributed

SELECT RegionID, uniqExact(UserID) AS u1, uniqUpTo(10)(UserID) AS u2 FROM remote('127.0.0.{1,2}', test, visits) GROUP BY RegionID HAVING u1 <= 11 AND u1 != u2
