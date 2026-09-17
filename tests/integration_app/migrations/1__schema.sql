CREATE TABLE Department (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE Person (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    age INTEGER,
    score BIGINT,
    weight DOUBLE PRECISION,
    active BOOLEAN,
    department_id INTEGER REFERENCES Department(id)
);
