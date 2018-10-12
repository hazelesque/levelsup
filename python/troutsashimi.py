#!/usr/bin/env python

from __future__ import with_statement
import sys
import re

RE_INSTNAME = re.compile(r"^i (?P<instname>.*)$")
RE_USER = re.compile(r"^u (?P<username>[a-z]*)$")
RE_FRIEND = re.compile(r"^fr (?P<u1>[a-z]*) (?P<u2>[a-z]*)$")
RE_DISTQUERY = re.compile(r"^dq (?P<u1>[a-z]*) (?P<u2>[a-z]*)$")


class UsernameAlreadyTakenException(ValueError):
    pass


class UserNotInThisInstanceException(ValueError):
    pass


class TroutSashimiInstance(object):
    users = None
    instname = None

    def __init__(self, instname=None):
        self.users = dict()
        self.instname = instname

    def register(self, user):
        if user.username not in self.users:
            self.users[user.username] = user
        else:
            raise UsernameAlreadyTakenException()

    def distance_between(self, u1, u2, path=None):
        if (u1.instance_ != self) or (u2.instance_ != self):
            raise UserNotInThisInstanceException()

        if path is None:
            path = list() # use this to detect loops

        path_updated = path + [u1.username]

        if u1 == u2:
            return 0

        best_distance = None

        for fr_username in u1.friends:
            fr = self.users[fr_username]

            if fr_username in path_updated:
                continue # we've seen this node before in this path

            tmp_dist = self.distance_between(fr, u2, path=path_updated)

            if tmp_dist is not None:
                tmp_dist += 1

                if best_distance is None:
                    best_distance = tmp_dist
                elif best_distance > tmp_dist:
                    best_distance = tmp_dist

            if best_distance == 1:
                # u1 is directly connected to u2, there will be no shorter path from current value of u1 to u2
                break

        return best_distance


class TroutSashimiUser(object):
    username = None
    friends = None
    instance_ = None # unsure if "instance" is a reserved keyword in Python

    def __init__(self, instance_, username):
        self.username = username
        self.friends = set()
        self.instance_ = instance_

        self.instance_.register(self)

    def addfriend(self, fr):
        if fr.instance_ != self.instance_:
            raise UserNotInThisInstanceException()

        self.friends.add(fr.username)
        fr.friends.add(self.username)


def main():
    for filename in sys.argv[1:]:
        with open(filename) as f:
            print "********************************"
            print "Processing instance file %s." % (filename,)

            inst = TroutSashimiInstance()

            for line in f:
                line = line.rstrip("\n\r")

                m = RE_INSTNAME.match(line)

                if m:
                    inst.instname = m.group('instname')

                    print "Instance name is %s." % (inst.instname,)

                    continue

                m = RE_USER.match(line)

                if m:
                    u = TroutSashimiUser(inst, m.group('username'))

                    print "Added user %s." % (u.username,)

                    continue

                m = RE_FRIEND.match(line)

                if m:
                    u1_name = m.group('u1')
                    u2_name = m.group('u2')

                    u1 = inst.users[u1_name]
                    u2 = inst.users[u2_name]

                    u1.addfriend(u2)

                    print "Added friendship between %s and %s." % (u1_name, u2_name,)

                    continue

                m = RE_DISTQUERY.match(line)

                if m:
                    u1_name = m.group('u1')
                    u2_name = m.group('u2')

                    u1 = inst.users[u1_name]
                    u2 = inst.users[u2_name]

                    dist = inst.distance_between(u1, u2)

                    if dist:
                        print "Distance between %s and %s: %d." % (u1_name, u2_name, dist,)
                    else:
                        print "No path between %s and %s." % (u1_name, u2_name,)

                    continue


if __name__ == "__main__":
    main()
