'use client';

import { collection, onSnapshot, orderBy, query } from "firebase/firestore";
import { useEffect, useState } from "react";
import { Session } from "./types";
import Link from "next/link";
import { db } from '@/lib/firebase';

export default function Home() {
  const [sessions, setSessions] = useState<Session[]>([]);
  useEffect(() => {
    return onSnapshot(query(collection(db, "sessions-2"),
      orderBy('last_update', 'desc')
    ), {
      next: (coll) => {
        setSessions(coll.docs.map(doc => {
          const session = doc.data();
          return { id: doc.id, ...session } as Session;
        }))
      }
    });
  }, []);
  return (
    <main className="p-4">
      <h2 className="text-2xl">Sessions</h2>
      <ul>
        {
          sessions.map((session) => {
            return <li key={session.id} className="bg-white m-2 p-2 rounded">
              <h1><Link href={"/sessions/" + session.id} className="text-xl">{session.id}</Link></h1>
              Last Update: {session.last_update.toDate().toString()}
            </li>
          })
        }
      </ul>
    </main>
  );
}
