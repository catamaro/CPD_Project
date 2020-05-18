/**************************Declarations**************************/
#include <mpi.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <omp.h>

#define RAND01 ((double)random() / (double)RAND_MAX)

typedef struct entry {
  int user;
  int item;
  double rate;
  int recom;
  struct entry *nextItem;
  struct entry *nextUser;
} entry;

 typedef struct group{
    // total count of number of entries
    int count;
    // first and last user of that group
    int firstUser;
    int lastUser;
    // list of machines for that group
    int *machines;
    // total machines
   int numMach;
  } group;

void alloc_A(int nU, int nI, entry ***_A_user, entry ***_A_item,
             entry ***_A_user_aux, entry ***_A_item_aux);

void update_LR(double ***L, double ***R, double ***newL, double ***newR);

entry *createNode();

void update_recom(int nU, int k0, int k1, double ***L, double ***R,
                 entry ***A_user, double *B, double *group_B, MPI_Comm group_comm, int sizeof_B);

void alloc_LR(int nU, int nI, int nF, double ***L, double ***R, double ***newL,
               double ***newR);

void alloc_B(int nU, int nI, double ***B);

void random_fill_LR(int user_i, int n_users, int nU, int nI, int k0, int n_feat, int nF, double ***L, double ***R,
                    double ***newL, double ***newR);

entry** split_A(entry ***_A_user, entry ***_A_item, int new_first_user, int interval, int nU, int nI);

void free_LR(int nU, int nF, double ***L, double ***R, double ***newL,
             double ***newR);

void free_B(int nU, double ***B);

/****************************************************************/

int main(int argc, char *argv[]) {
  FILE *fp;
  int nIter, nFeat, nUser, nItem, nEntry, nUser_original;
  int *solution;
  double deriv = 0;
  double alpha, sol_aux;
  double **L, **R, **B, **newL, **newR, *local_B, *group_B, *local_B_final, *group_B_final;
  double elapsed_time;
  int my_group;

  entry **A_user, **A_user_aux, **A_item, **A_item_aux;
  entry *A_aux1, *A_aux2;

  //MPI_Status status; ??
  int provided;

	MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED , &provided );
  MPI_Barrier(MPI_COMM_WORLD);

  elapsed_time = -MPI_Wtime();
  
  int world_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  
  int world_size;
  MPI_Comm_size (MPI_COMM_WORLD, &world_size);
  
  char processor_name[MPI_MAX_PROCESSOR_NAME];
  int name_len;
  MPI_Get_processor_name(processor_name, &name_len);

  if (argc != 2) {
    printf("error: command of type ./matFact <filename.in>\n");
    MPI_Finalize();
    exit(1);
  }

  fp = fopen(argv[1], "r");
  if (fp == NULL) {
    printf("error: cannot open file\n");
    MPI_Finalize();
    exit(1);
  }

  /******************************Setup******************************/
  // read of first parameters of file
  fscanf(fp, "%d", &nIter);
  fscanf(fp, "%lf", &alpha);
  fscanf(fp, "%d", &nFeat);
  fscanf(fp, "%d %d %d", &nUser, &nItem, &nEntry);
  nUser_original=nUser;

  // alloc struct that holds A and it's approximation, B
  alloc_A(nUser, nItem, &A_user, &A_item, &A_user_aux, &A_item_aux);

  // alloc vector that holds highest recom. per user
  solution = calloc(sizeof(int), nUser);

  // vector with number of items per user
  int *count = (int*)calloc(sizeof(int), nUser);
  int auxUser = 0, userIdx = 0;

  // construct of a list of lists
  for (int i = 0; i < nEntry; i++) {
    A_aux1 = createNode();
    // load of entryAof matrix A
    fscanf(fp, "%d %d %lf", &(A_aux1->user), &(A_aux1->item), &(A_aux1->rate));

    // store the sum of items per user
    if(auxUser == A_aux1->user){
      count[userIdx] ++;
    }
    else{
      auxUser = A_aux1->user;
      userIdx++;
      count[userIdx] ++;
    }

    if (A_user[A_aux1->user] == NULL) {

      A_user[A_aux1->user] = A_aux1;
      A_user_aux[A_aux1->user] = A_aux1;

    } else {

      A_user_aux[A_aux1->user]->nextItem = A_aux1;
      A_user_aux[A_aux1->user] = A_aux1;
    }

    if (A_item[A_aux1->item] == NULL) {

      A_item[A_aux1->item] = A_aux1;
      A_item_aux[A_aux1->item] = A_aux1;

    } else {

      A_item_aux[A_aux1->item]->nextUser = A_aux1;
      A_item_aux[A_aux1->item] = A_aux1;
    }
  }

  fclose(fp);
  free(A_item_aux);
  free(A_user_aux);

  /****************************End Setup****************************/

  /*******************Code for load balance********************/
  // number of groups
  int div = floor(sqrt(world_size));
  // 
  while(world_size % div != 0) div--;
  // number of entries per division rounded down
  int lower = nEntry / div;
  // number of entries per division plus one
  int upper = nEntry / div + 1;
  // rest of division
  int rest =  div % nEntry;
  
  int aux = 0, j = 0;

  // vector with groups of users
  group *groups = (group*)calloc(sizeof(group), div);

  for(int i = 0; i < nUser; i++){
    // save first user of the group
    if(groups[j].count == 0) groups[j].firstUser = i;

    // if we are in the last partition it gets the remaing users
    if(j == div-1) groups[j].count += count[i];
    else{
      // aux that stores the possibel new user for that group
      aux = groups[j].count + count[i];

      // if the lower threshold is not with the sum of the new user add it and continue
      if(aux <= lower) groups[j].count = aux;
      // if threshold is atchived
      else if((aux - upper) >= 0){
        // check to see where the difference betwhen the two threshold
        // is less
        if(abs(groups[j].count - lower) < (aux - upper)){
          // add the difference between the desired threshold
          rest += aux - lower;
          // save last user of the group
          groups[j].lastUser = i-1;
          // advance to next group
          j++;
          // the count that wasnt considered becaused it was closer to the lower boundary
          // is put automatically in the next group
          groups[j].count = count[i];
          // save first user of the group
          groups[j].firstUser = i;
        }
        // check to see if there were already other groups to be above the upper threshold
        // decrising already the rest
        else if(rest > (aux - upper)){
          // subtract the difference between the desired threshold
          rest -= aux - upper;
          // add the final user for that group
          groups[j].count = aux;
          // save last user of the group
          groups[j].lastUser = i;
          // advance to next group
          j++;
        }
        // if so, it does not add the next user to the group
        else{
          // add the difference between the desired threshold
          rest += aux - lower;
          // add the final user for that group
          groups[j].count = aux;
          // save last user of the group
          groups[j].lastUser = i;
          // advance to next group
          j++;
        }
      }
    }
    // if we are in the last iteration
    if(i+1 == nUser) groups[j].lastUser = i;
  }

  int divMachine = world_size / div;
  int k = 0;
  
  // assigns a each group a set of computers
  for(int i = 0; i < div; i++){
    groups[i].machines = malloc(sizeof(int) * divMachine);
    groups[i].numMach = divMachine;

    for(int j = 0; j < divMachine; j++){
      // if de problem in not divisible by the number of computers sets
      // to the maximun possible meaning the group with zero rows are not 
      // assinged to any machine
      if(groups[i].count != 0){
        groups[i].machines[j] = k;
        if(k == world_rank) my_group = i;
        k++;
      }
      else break;
    }
  }

  /*******************Code for load balance********************/

  MPI_Comm B_comm, R_comm;
  MPI_Group world_group, B_group, R_group;

  MPI_Comm_group(MPI_COMM_WORLD, &world_group);

  int k0, k1;
  int counter = 0;
  int user_i = 0;
  int user_l = 0;

  int *rank = groups[my_group].machines;

  MPI_Group_incl(world_group, groups[my_group].numMach, rank, &B_group);
  MPI_Comm_create_group(MPI_COMM_WORLD, B_group, 0, &B_comm);

  nUser = groups[my_group].lastUser - groups[my_group].firstUser + 1;
  user_i = groups[my_group].firstUser;

  A_user = split_A(&A_user, &A_item, user_i, nUser, nUser_original, nItem);

  for (int i = 0; i < nUser; i++) {
    A_aux1 = A_user[i];

    while(A_aux1 != NULL){
      A_aux1->recom = counter;
      counter++;

      A_aux1 = A_aux1->nextItem;
    }
  }
  
  local_B = malloc(sizeof(double) * counter+1);
  group_B = malloc(sizeof(double) * counter+1);

  group *R_groups = (group*)calloc(sizeof(group), groups[my_group].numMach);

  // lower num of machines per group
  int lowerMachine = nFeat/groups[my_group].numMach;
  // upper num of machines per group
  int upperMachine = lowerMachine + 1;
  // only use upper if rest of division not zero
  int restMachine =  nFeat % groups[my_group].numMach;

  divMachine = upperMachine;

  k = 0;

  int my_R_group;

  for(int i = 0; i < groups[my_group].numMach; i++){
    
    if(i >= restMachine) divMachine = lowerMachine;

    if(i != 0) k = R_groups[i-1].lastUser;

    R_groups[i].machines = malloc(sizeof(int) * div);
    R_groups[i].firstUser = k;
    R_groups[i].lastUser = R_groups[i].firstUser + divMachine;

    #pragma omp parallel default(none) shared(groups, R_groups, my_R_group, i, world_rank, div)
    {
      #pragma omp for
      for(int j = 0; j < div; j++){
        R_groups[i].machines[j] = groups[j].machines[i];
        if(world_rank == R_groups[i].machines[j]) my_R_group = i;
      }
    }
  }

  k0 = R_groups[my_R_group].firstUser;
  k1 = R_groups[my_R_group].lastUser;
  MPI_Group_incl(world_group, div, R_groups[my_R_group].machines, &R_group);
  MPI_Comm_create_group(MPI_COMM_WORLD, R_group, 0, &R_comm);

  int inter = k1-k0;
  int sizeof_derivs = (inter*nItem);
  double *derivs = malloc(sizeof(double) * sizeof_derivs);
  double *global_derivs = malloc(sizeof(double) * sizeof_derivs);
  int nFeat_original = nFeat;

  nFeat = inter;
  alloc_LR(nUser, nItem, nFeat, &L, &R, &newL, &newR);
  random_fill_LR(user_i, nUser, nUser_original, nItem, k0, nFeat, nFeat_original, &L, &R, &newL, &newR);


  for (int n = 0; n < nIter; n++) {

    update_recom(nUser, k0, k1, &L, &R, &A_user, local_B, group_B, B_comm, counter+1);

   #pragma omp parallel default(none) shared(nUser, nFeat, A_user, R, L, newL, alpha, group_B, k0, k1) private(A_aux1, deriv)
    {
      //L+1
      #pragma omp for schedule(dynamic)   ///?????????????
      for (int i = 0; i < nUser; i++) {
        for (int k = 0; k < nFeat; k++) {

          A_aux1 = A_user[i];
          // sum of derivatives per item

          while (A_aux1 != NULL) {
            deriv +=
                2 * (A_aux1->rate - group_B[A_aux1->recom]) * (-R[k][A_aux1->item]);
            A_aux1 = A_aux1->nextItem;
          }
          // final calculation of t+1
          A_aux1 = A_user[i];
          if(A_aux1 != NULL)
            newL[i][k] = L[i][k] - alpha * deriv;
          deriv = 0;
        }
      }
    }

    //R+1
    int c = 0;
    for (int j = 0; j < nItem; j++) {
      for (int k = 0; k < nFeat; k++) {

        A_aux1 = A_item[j];

        // sum of derivatives per user
        while (A_aux1 != NULL) {
          deriv += 2 * (A_aux1->rate - group_B[A_aux1->recom]) * (-L[A_aux1->user - user_i][k]);
          A_aux1 = A_aux1->nextUser;
        }
        // final calculation of t+1
        derivs[c] = alpha * deriv;
        c++;
        deriv = 0;
      }
    }
    
    MPI_Allreduce(derivs, global_derivs, sizeof_derivs, MPI_DOUBLE, MPI_SUM, R_comm);

    c = 0;
    for (int j = 0; j < nItem; j++) {
      for (int k = 0; k < nFeat; k++) {
        newR[k][j] = R[k][j] - global_derivs[c];
        c++;
        }
    }

    update_LR(&L, &R, &newL, &newR);
  }


  local_B_final = malloc(sizeof(double) * nUser * nItem);
  group_B_final = malloc(sizeof(double) * nUser * nItem);

  int c = 0;
  for (int i = 0; i < nUser; i++) {
    for (int j = 0; j < nItem; j++) {
      local_B_final[c] = 0;
      group_B_final[c] = 0;
      for (int k = 0; k < nFeat; k++){
        local_B_final[c] += L[i][k] * R[k][j];
      }
      c++;
    }
  }

  MPI_Reduce(local_B_final, group_B_final, nUser * nItem, MPI_DOUBLE, MPI_SUM, 0, B_comm);

  free_LR(nUser, nFeat, &L, &R, &newL, &newR);
  alloc_B(nUser, nItem, &B);

  c = 0;
  for(int i = 0; i < nUser; i++) {
    for(int j = 0; j < nItem; j++) {
      B[i][j] = group_B_final[c];
      c++;
    }
  }

  for (int k = 0; k < nUser; k++) {
    sol_aux = 0;
    A_aux1 = A_user[k];

    // update entry of B to 0 if item already rated
    while (A_aux1 != NULL) {
      B[A_aux1->user - user_i][A_aux1->item] = 0;
      A_aux1 = A_aux1->nextItem;
    }

    user_l = user_i + k;
    // save item with highest rate
    for(int j = 0; j < nItem; j++){
      if (B[k][j] > sol_aux) {
        solution[user_l] = j;
        sol_aux = B[k][j];
      }
    }
  }

  int *global_solution;
  global_solution = calloc(sizeof(int), nUser_original);

  if(my_R_group == 0){
    MPI_Reduce(solution, global_solution, nUser_original, MPI_INT, MPI_SUM, 0, R_comm);
  }

  if(world_rank == 0){
    for(int i =0; i < nUser_original; i++)
      printf("%d\n", global_solution[i]);
  }

  /******************************Free A*****************************/
  for (int i = 0; i < nUser; i++) {
    
    A_aux1 = A_user[i];

    while (A_aux1 != NULL) {
      A_aux2 = A_aux1->nextItem;
      free(A_aux1);
      A_aux1 = A_aux2;
    }
  }
  free(A_user);
  free(A_item);

  free(count);
  free(solution);
  
  free(global_solution);

  free_B(nUser, &B);

  free(local_B);
  free(group_B);
  free(local_B_final);
  free(group_B_final);
  

  for (int i = 0; i < groups[my_group].numMach; i++)
  {
    free(R_groups[i].machines);
  }
  free(R_groups);

  for (int i = 0; i < div; i++)
  {
    free(groups[i].machines);
  }
  free(groups);

  free(derivs);
  free(global_derivs);

  printf("Finalizing\n");
  elapsed_time += MPI_Wtime();
  printf("elapsed_time: %.1f\n", elapsed_time);

  MPI_Finalize();

  return 0;
}

void alloc_A(int nU, int nI, entry ***_A_user, entry ***_A_item, entry ***_A_user_aux, entry ***_A_item_aux) {

  *_A_user = (entry **)calloc(sizeof(entry *), nU);
  *_A_item = (entry **)calloc(sizeof(entry *), nI);

  *_A_user_aux = (entry **)calloc(sizeof(entry *), nU);
  *_A_item_aux = (entry **)calloc(sizeof(entry *), nI);
}

entry *createNode() {

  entry *A;
  A = (entry *)malloc(sizeof(entry));
  A->nextItem = NULL;
  A->nextUser = NULL;

  return A;
}

entry** split_A(entry ***_A_user, entry ***_A_item, int new_first_user, int interval, int nU, int nI){

  entry **newU;
  entry *aux;
  entry *aux1;
  int x=0;
  //percorrer o item para ter a certeza que todas as heads estão dentro da zona permitida
  #pragma omp parallel default(none) shared(_A_user, _A_item, new_first_user, interval, nU, nI, newU, x) private(aux, aux1)
	{ 
    #pragma omp for
    for (int i = 0; i < nI; i++)
    {
      if(((*_A_item)[i]) != NULL){
        if((((*_A_item)[i])->user)<new_first_user){

          while((((*_A_item)[i])->user)<new_first_user){ //percorre até ultrapassar o valor do primeiro user

            (*_A_item)[i] = ((*_A_item)[i])->nextUser;
            if((*_A_item)[i] == NULL)
              break;
          }

          if((*_A_item)[i] != NULL) {
            if((((*_A_item)[i])->user)>=new_first_user+interval){ //ultrapassa o target
              /* meter a NULL */
              (*_A_item)[i] = NULL;
            }
          }


          }
        else if ((((*_A_item)[i])->user)>=new_first_user+interval){

          /* meter a NULL */
          (*_A_item)[i] = NULL;
        }
      }

      if((*_A_item)[i] != NULL) {
            aux1 = (*_A_item)[i];
            while(aux1->nextUser != NULL) {
              if(aux1->nextUser->user >= new_first_user+interval){
                aux1->nextUser = NULL;
              }
              else {
                aux1 = aux1->nextUser;
              }
            }
        }
        //else está dentro do target
    }
  }
  
  //allocar um novo array para os users com a dimensao correcta
  newU = (entry **)calloc(sizeof(entry *), interval);


  //copiar parte do array total para o novo array

  for (int i = 0; i < nU; i++)
  {
      if((i<new_first_user) || (i>=new_first_user+interval)){ //fora do target

        while(((*_A_user)[i])!=NULL){  //free das estruturas da lista

          aux = ((*_A_user)[i])->nextItem;
          free((*_A_user)[i]);
          (*_A_user)[i] = aux;
        }

      }
      else{ //dentro do target

        // meter a NULL
        newU[x]=(*_A_user)[i];
        x++;
        (*_A_user)[i] = NULL;
      }
  }
  
  //dar free do resto do array velho
  free(*_A_user);

  return newU;
}

void free_LR(int nU, int nF, double ***L, double ***R, double ***newL, double ***newR) {

  for (int i = 0; i < nU; i++) {
    free((*L)[i]);
    free((*newL)[i]);
  }
  free(*L);
  free(*newL);

  for (int i = 0; i < nF; i++) {
    free((*R)[i]);
    free((*newR)[i]);
  }
  free(*newR);
  free(*R);
}

void free_B(int nU, double ***B) {
  for (int i = 0; i < nU; i++) {
    free((*B)[i]);
  }
  free(*B);
}

void update_recom(int nU, int k0, int k1, double ***L, double ***R, entry ***A_user, double *B, double *group_B, MPI_Comm group_comm, int sizeof_B) {
  entry *A_aux1;
  int nFeat = k1 - k0;
  // update recomendation for all non-zero entries meaning
  // the approximation of B to A

  #pragma omp parallel default(none) shared(nU, A_user, B, group_B, k0, k1, L, R, nFeat) private(A_aux1)
  { 
    #pragma omp for
    for (int i = 0; i < nU; i++) {
      A_aux1 = (*A_user)[i];
      while (A_aux1 != NULL) {
        B[A_aux1->recom] = 0;
        group_B[A_aux1->recom] = 0;

        for (int k = 0; k < nFeat; k++){
          B[A_aux1->recom] += (*L)[i][k] * (*R)[k][A_aux1->item];
        }

        A_aux1 = A_aux1->nextItem;
      }
    }
  }

  

  MPI_Allreduce(B, group_B, sizeof_B, MPI_DOUBLE, MPI_SUM, group_comm);
}

void alloc_LR(int nU, int nI, int nF, double ***L, double ***R, double ***newL, double ***newR) { //OMP

  *L = (double **)malloc(sizeof(double *) * nU);
  *newL = (double **)malloc(sizeof(double *) * nU);
  *R = (double **)malloc(sizeof(double *) * nF);
  *newR = (double **)malloc(sizeof(double *) * nF);

  // parallelized section
  #pragma omp parallel default(none) shared(L, newL, R, newR, nU, nF, nI)
  {

    // parallelized allocation with nowait because there 
    // are no dependencies in the next loop
    #pragma omp for nowait
    for (int i = 0; i < nU; i++) {
      (*L)[i] = (double *)malloc(sizeof(double) * nF);
      (*newL)[i] = (double *)malloc(sizeof(double) * nF);
    }

    // parallelized allocation without nowait due to
    // synchronization in the end of the section
    #pragma omp for
    for (int i = 0; i < nF; i++) {
      (*R)[i] = (double *)malloc(sizeof(double) * nI);
      (*newR)[i] = (double *)malloc(sizeof(double) * nI);
    }
  }
}

void alloc_B(int nU, int nI, double ***B) { //OMP

  *B = (double **)malloc(sizeof(double *) * nU);

  // parallelized section
  #pragma omp parallel default(none) shared(B, nU, nI)
  {

    // parallelized allocation with nowait because there
    // are no dependencies in the next loop
    #pragma omp for
    for (int i = 0; i < nU; i++) {
      (*B)[i] = (double *)malloc(sizeof(double) * nI);
    }

  }
}


void random_fill_LR(int user_i, int n_users, int nU, int nI, int k0, int n_feat, int nF, double ***L, double ***R,
                    double ***newL, double ***newR) {
  srandom(0);
  int user_l = user_i + n_users;
  int k1 = k0 + n_feat;
  //double garbage;

  // init of L, stable version, and newL for t+1
  for (int i = 0; i < nU; i++)
    for (int j = 0; j < nF; j++) {
      if(i >= user_i && i < user_l && j >= k0 && j < k1) {
        (*L)[i - user_i][j - k0] = RAND01 / (double)nF;
        (*newL)[i - user_i][j - k0] = (*L)[i - user_i][j - k0];
      }
      else{
        random();
      }
    }

  // init of R, stable version, and newR for t+1
  for (int i = 0; i < nF; i++)
    for (int j = 0; j < nI; j++) {
      if(i >= k0 && i < k1) {
        (*R)[i - k0][j] = RAND01 / (double)nF;
        (*newR)[i - k0][j] = (*R)[i - k0][j];
      }
      else {
        random();
      }
    }
}

void update_LR(double ***L, double ***R, double ***newL, double ***newR) {//OMP

  double **aux;

  // update stable version of L with L(t+1) by switching
  // the pointers
  aux = *L;
  *L = *newL;
  *newL = aux;

  // update stable version of R with R(t+1) by switching
  // the pointers
  aux = *R;
  *R = *newR;
  *newR = aux;
}
