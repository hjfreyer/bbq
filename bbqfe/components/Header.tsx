'use client'
import React, { useState, useEffect } from "react";
import Link from "next/link";
import {
	signInWithGoogle,
	signOut,
	onAuthStateChanged
} from "@/lib/auth";
// import { addFakeRestaurantsAndReviews } from "@/lib/firebase/firestore.js";
import { useRouter } from "next/navigation";

import { collection, doc, query, orderBy, Firestore, getFirestore, getDoc, onSnapshot, limit, Timestamp } from 'firebase/firestore';


const db = getFirestore();

function useUserSession(initialUser) {
	// The initialUser comes from the server via a server component
	const [user, setUser] = useState(initialUser);
	const router = useRouter()

	useEffect(() => {
		const unsubscribe = onAuthStateChanged((authUser) => {
			setUser(authUser)
		})

		return () => unsubscribe()
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [])

	useEffect(() => {
		onAuthStateChanged((authUser) => {
			if (user === undefined) return

			// refresh when user changed to ease testing
			if (user?.email !== authUser?.email) {
				router.refresh()
			}
		})
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [user])

	return user;
}

export default function Header({initialUser}) {

	const user = useUserSession(initialUser) ;

	const handleSignOut = event => {
		event.preventDefault();
		signOut();
	};

	const handleSignIn = event => {
		event.preventDefault();
		signInWithGoogle();
	};


    const [sessions, setSessions] = useState([]);
    useEffect(() => {
        return onSnapshot(query(collection(db, "sessions"),
                         orderBy('last_update', 'desc')
                        ), {
            next: (coll) => {


                setSessions(coll.docs.map(doc => {
                    const session = doc.data();

                    // session.samples = _.sortBy(session.samples, 'time.seconds')

                    return [doc.id, session];
                }))
            }
        });
    }, []);
	return (
		<header>
			<Link href="/" className="logo">
				UNFriendly Eats
			</Link>

            {
                sessions.map(([id, session]) => {
                    return <div key={id}>
<pre>{JSON.stringify(session)}</pre>

                        {/* <h1><Link to={"/sessions/" + id}>{id}</Link></h1>
                        Last Update: {session.last_update.toDate().toString()}
                        Group: {JSON.stringify(session.group)} */}
                    </div>
                })
            }
			{user ? (
				<>
					<div className="profile">
						<p>
							<img src="/profile.svg" alt={user.email} />
							{user.displayName}
						</p>

						<div className="menu">
							...
							<ul>
								<li>{user.displayName}</li>

								{/* <li>
									<a
										href="#"
										onClick={addFakeRestaurantsAndReviews}
									>
										Add sample restaurants
									</a>
								</li> */}

								<li>
									<a href="#" onClick={handleSignOut}>
										Sign Out
									</a>
								</li>
							</ul>
						</div>
					</div>
				</>
			) : (
				<a href="#" onClick={handleSignIn}>
					Sign In with Google
				</a>
			)}
		</header>
	);
}