import { Timestamp } from "firebase/firestore";

export type Session = {
    id: string,
    last_update: Timestamp,
}