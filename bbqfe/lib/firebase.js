
import { initializeApp } from "firebase/app";
import { getFirestore } from "firebase/firestore";

export const firebaseConfig = {
  apiKey: "AIzaSyDeu9w87YqHFFOen3iAoyCpFPz0HU52Hxw",
  authDomain: "hjfreyer-bbq.firebaseapp.com",
  projectId: "hjfreyer-bbq",
  storageBucket: "hjfreyer-bbq.appspot.com",
  messagingSenderId: "136827981042",
  appId: "1:136827981042:web:d050ec669b77cc920c546b"
};

export const firebaseApp = initializeApp(firebaseConfig);
export const db = getFirestore(firebaseApp);